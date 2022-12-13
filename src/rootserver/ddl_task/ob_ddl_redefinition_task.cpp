/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX RS
#include "ob_ddl_redefinition_task.h"
#include "lib/rc/context.h"
#include "rootserver/ddl_task/ob_constraint_task.h"
#include "rootserver/ddl_task/ob_ddl_scheduler.h"
#include "rootserver/ddl_task/ob_modify_autoinc_task.h"
#include "rootserver/ob_root_service.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/ob_ddl_error_message_table_operator.h"
#include "share/ob_autoincrement_service.h"
#include "share/ob_ddl_checksum.h"
#include "storage/tablelock/ob_table_lock_service.h"
#include "storage/tablelock/ob_table_lock_rpc_client.h"
using namespace oceanbase::lib;
using namespace oceanbase::common;
using namespace oceanbase::common::hash;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;
using namespace oceanbase::rootserver;
using namespace oceanbase::transaction::tablelock;

ObDDLRedefinitionSSTableBuildTask::ObDDLRedefinitionSSTableBuildTask(
    const int64_t task_id,
    const uint64_t tenant_id,
    const int64_t data_table_id,
    const int64_t dest_table_id,
    const int64_t schema_version,
    const int64_t snapshot_version,
    const int64_t execution_id,
    const ObSQLMode &sql_mode,
    const common::ObCurTraceId::TraceId &trace_id,
    const int64_t parallelism,
    const bool use_heap_table_ddl_plan,
    ObRootService *root_service)
  : is_inited_(false), tenant_id_(tenant_id), task_id_(task_id), data_table_id_(data_table_id),
    dest_table_id_(dest_table_id), schema_version_(schema_version), snapshot_version_(snapshot_version),
    execution_id_(execution_id), sql_mode_(sql_mode), trace_id_(trace_id), parallelism_(parallelism),
    use_heap_table_ddl_plan_(use_heap_table_ddl_plan), root_service_(root_service)
{
  set_retry_times(0); // do not retry
}

int ObDDLRedefinitionSSTableBuildTask::init(
    const ObTableSchema &orig_table_schema,
    const AlterTableSchema &alter_table_schema,
    const ObTimeZoneInfoWrap &tz_info_wrap)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
    LOG_WARN("init twice", K(ret));
  } else if (OB_FAIL(tz_info_wrap_.deep_copy(tz_info_wrap))) {
    LOG_WARN("fail to copy time zone info wrap", K(ret), K(tz_info_wrap));
  } else if (OB_FAIL(col_name_map_.init(orig_table_schema, alter_table_schema))) {
    LOG_WARN("failed to init column name map", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObDDLRedefinitionSSTableBuildTask::process()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObTabletID unused_tablet_id;
  ObTraceIdGuard trace_id_guard(trace_id_);
  ObSqlString sql_string;
  ObSchemaGetterGuard schema_guard;
  const ObSysVariableSchema *sys_variable_schema = nullptr;
  ObDDLTaskKey task_key(dest_table_id_, schema_version_);
  bool oracle_mode = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl redefinition sstable build task not inited", K(ret));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(
      tenant_id_, schema_guard))) {
    LOG_WARN("fail to get tenant schema guard", K(ret), K(data_table_id_));
  } else if (OB_FAIL(schema_guard.check_formal_guard())) {
    LOG_WARN("fail to check formal guard", K(ret));
  } else if (OB_FAIL(schema_guard.get_sys_variable_schema(
      tenant_id_, sys_variable_schema))) {
    LOG_WARN("get sys variable schema failed", K(ret), K(tenant_id_));
  } else if (OB_ISNULL(sys_variable_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sys variable schema is NULL", K(ret));
  } else if (OB_FAIL(sys_variable_schema->get_oracle_mode(oracle_mode))) {
    LOG_WARN("get oracle mode failed", K(ret));
  } else if (OB_FAIL(ObDDLUtil::generate_build_replica_sql(tenant_id_,
                                                           data_table_id_,
                                                           dest_table_id_,
                                                           schema_version_,
                                                           snapshot_version_,
                                                           execution_id_,
                                                           task_id_,
                                                           parallelism_,
                                                           use_heap_table_ddl_plan_,
                                                           false/*use_schema_version_hint_for_src_table*/,
                                                           &col_name_map_,
                                                           sql_string))) {
    LOG_WARN("fail to generate build replica sql", K(ret));
  } else {
    ObTimeoutCtx timeout_ctx;
    common::ObCommonSqlProxy *user_sql_proxy = nullptr;
    int64_t affected_rows = 0;
    if (oracle_mode) {
      sql_mode_ = SMO_STRICT_ALL_TABLES | SMO_PAD_CHAR_TO_FULL_LENGTH;
    }
    ObSessionParam session_param;
    session_param.sql_mode_ = reinterpret_cast<int64_t *>(&sql_mode_);
    session_param.tz_info_wrap_ = &tz_info_wrap_;
    session_param.ddl_info_.set_is_ddl(true);
    session_param.ddl_info_.set_source_table_hidden(false);
    session_param.ddl_info_.set_dest_table_hidden(true);
    session_param.ddl_info_.set_heap_table_ddl(use_heap_table_ddl_plan_);
    if (oracle_mode) {
      user_sql_proxy = GCTX.ddl_oracle_sql_proxy_;
    } else {
      user_sql_proxy = GCTX.ddl_sql_proxy_;
    }
    LOG_INFO("execute sql" , K(sql_string), K(data_table_id_), K(tenant_id_),
             "is_strict_mode", is_strict_mode(sql_mode_), K(sql_mode_), K(parallelism_));
    if (OB_FAIL(timeout_ctx.set_trx_timeout_us(OB_MAX_DDL_SINGLE_REPLICA_BUILD_TIMEOUT))) {
      LOG_WARN("set trx timeout failed", K(ret));
    } else if (OB_FAIL(timeout_ctx.set_timeout(OB_MAX_DDL_SINGLE_REPLICA_BUILD_TIMEOUT))) {
      LOG_WARN("set timeout failed", K(ret));
    } else {
      if (OB_FAIL(user_sql_proxy->write(tenant_id_, sql_string.ptr(), affected_rows,
              oracle_mode ? ObCompatibilityMode::ORACLE_MODE : ObCompatibilityMode::MYSQL_MODE, &session_param))) {
        LOG_WARN("fail to execute build replica sql", K(ret), K(tenant_id_));
      }
    }
  }
  if (OB_SUCCESS != (tmp_ret = root_service_->get_ddl_scheduler().on_sstable_complement_job_reply(unused_tablet_id, task_key, snapshot_version_, execution_id_, ret))) {
    LOG_WARN("fail to finish sstable complement", K(ret));
  }
  return ret;
}

ObAsyncTask *ObDDLRedefinitionSSTableBuildTask::deep_copy(char *buf, const int64_t buf_size) const
{
  int ret = OB_SUCCESS;
  ObDDLRedefinitionSSTableBuildTask *new_task = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ddl redefinition sstable build task not inited", K(ret));
  } else if (OB_UNLIKELY(nullptr == buf || buf_size < get_deep_copy_size())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(buf), K(buf_size), "required_deep_copy_size", get_deep_copy_size());
  } else {
    new_task = new (buf) ObDDLRedefinitionSSTableBuildTask(
        task_id_,
        tenant_id_,
        data_table_id_,
        dest_table_id_,
        schema_version_,
        snapshot_version_,
        execution_id_,
        sql_mode_,
        trace_id_,
        parallelism_,
        use_heap_table_ddl_plan_,
        root_service_);
    if (OB_FAIL(new_task->tz_info_wrap_.deep_copy(tz_info_wrap_))) {
      LOG_WARN("failed to copy tz info wrap", K(ret));
    } else if (OB_FAIL(new_task->col_name_map_.assign(col_name_map_))) {
      LOG_WARN("failed to assign column name map", K(ret));
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("fail to init new task", K(ret));
      new_task->~ObDDLRedefinitionSSTableBuildTask();
      new_task = nullptr;
    } else {
      new_task->is_inited_ = true;
    }
  }
  return new_task;
}

int ObDDLRedefinitionTask::prepare(const ObDDLTaskStatus next_task_status)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  }
  // overwrite ret
  if (OB_FAIL(switch_status(next_task_status, ret))) {
    LOG_WARN("fail to switch status", K(ret));
  }
  return ret;
}

int ObDDLRedefinitionTask::lock_table(const ObDDLTaskStatus next_task_status)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  ObDDLTaskStatus new_status = ObDDLTaskStatus::LOCK_TABLE;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get tenant schema guard failed", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(object_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(target_object_id_));
  } else if (OB_ISNULL(data_table_schema) || OB_ISNULL(dest_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table not exist", K(ret), K(object_id_), K(target_object_id_), KP(data_table_schema), KP(dest_table_schema));
  } else if (data_table_schema->is_tmp_table() != dest_table_schema->is_tmp_table()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("table type is different", K(ret), K(data_table_schema->is_tmp_table()), K(dest_table_schema->is_tmp_table()));
  } else if (data_table_schema->is_tmp_table()) {
    // no need to lock table and unlock table.
  } else if (OB_FAIL(ObTableLockRpcClient::get_instance().lock_table(object_id_,
                                          EXCLUSIVE, schema_version_, 0, tenant_id_))) {
    if (!ObDDLUtil::is_table_lock_retry_ret_code(ret)) {
      LOG_WARN("lock source table failed", K(ret), K(object_id_));
    } else {
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
        ObTaskController::get().allow_next_syslog();
        LOG_INFO("cannot lock source table", K(ret), K(object_id_));
      }
    }
  } else if (OB_FAIL(ObTableLockRpcClient::get_instance().lock_table(target_object_id_,
                                          EXCLUSIVE, schema_version_, 0, tenant_id_))) {
    if (!ObDDLUtil::is_table_lock_retry_ret_code(ret)) {
      LOG_WARN("lock dest table failed", K(ret), K(target_object_id_));
    } else {
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
        ObTaskController::get().allow_next_syslog();
        LOG_INFO("cannot lock dest table", K(ret), K(target_object_id_));
      }
    }
  }
  DEBUG_SYNC(DDL_REDEFINITION_LOCK_TABLE);
  if (OB_FAIL(ret)) {
    ret = ObDDLUtil::is_table_lock_retry_ret_code(ret) ? OB_SUCCESS : ret;
  } else if (OB_FAIL(obtain_snapshot())) {
    if (OB_SNAPSHOT_DISCARDED == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("fail to obtain snapshot version", K(ret));
    }
  } else {
    new_status = next_task_status;
  }
  if (new_status == next_task_status || OB_FAIL(ret)) {
    if (OB_FAIL(switch_status(new_status, ret))) {
      LOG_WARN("fail to switch task status", K(ret));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::check_table_empty(const ObDDLTaskStatus next_task_status)
{
  int ret = OB_SUCCESS;
  bool need_check_table_empty = false;
  bool is_check_replica_end = false;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObTableRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(check_need_check_table_empty(need_check_table_empty))) {
    LOG_WARN("failed to check need check table empty", K(ret));
  } else if (need_check_table_empty) {
    if (OB_FAIL(check_check_table_empty_end(is_check_replica_end))) {
      LOG_WARN("check build replica end", K(ret));
    } else if (!is_check_replica_end && check_table_empty_job_time_ == 0) {
      ObCheckConstraintValidationTask task(tenant_id_, object_id_, -1/*constraint id*/, target_object_id_,
                                           schema_version_, trace_id_, task_id_, true/*check_table_empty*/,
                                           obrpc::ObAlterTableArg::AlterConstraintType::ADD_CONSTRAINT);
      if (OB_FAIL(root_service->submit_ddl_single_replica_build_task(task))) {
        LOG_WARN("submit ddl single replica build task failed", K(ret));
      } else {
        check_table_empty_job_time_ = ObTimeUtility::current_time();
        LOG_INFO("send check constraint request", K(object_id_), K(target_object_id_), K(schema_version_));
      }
    }
  }

  if (OB_FAIL(ret) || is_check_replica_end || !need_check_table_empty) {
    if (OB_FAIL(switch_status(next_task_status, ret))) {
      LOG_WARN("fail to switch status", K(ret));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::hold_snapshot(const int64_t snapshot_version)
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  ObSEArray<ObTabletID, 1> tablet_ids;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_UNLIKELY(snapshot_version < 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(snapshot_version));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get tenant schema guard failed", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(object_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(target_object_id_));
  } else if (OB_ISNULL(data_table_schema) || OB_ISNULL(dest_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table not exist", K(ret), K(object_id_), K(target_object_id_), KP(data_table_schema), KP(dest_table_schema));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, object_id_, tablet_ids))) {
    LOG_WARN("failed to get data table snapshot", K(ret));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, target_object_id_, tablet_ids))) {
    LOG_WARN("failed to get dest table snapshot", K(ret));
  } else if (data_table_schema->get_aux_lob_meta_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, data_table_schema->get_aux_lob_meta_tid(), tablet_ids))) {
    LOG_WARN("failed to get data lob meta table snapshot", K(ret));
  } else if (data_table_schema->get_aux_lob_piece_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, data_table_schema->get_aux_lob_piece_tid(), tablet_ids))) {
    LOG_WARN("failed to get data lob piece table snapshot", K(ret));
  } else if (dest_table_schema->get_aux_lob_meta_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, dest_table_schema->get_aux_lob_meta_tid(), tablet_ids))) {
    LOG_WARN("failed to get dest lob meta table snapshot", K(ret));
  } else if (dest_table_schema->get_aux_lob_piece_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, dest_table_schema->get_aux_lob_piece_tid(), tablet_ids))) {
    LOG_WARN("failed to get dest lob piece table snapshot", K(ret));
  } else {
    ObDDLService &ddl_service = root_service->get_ddl_service();
    if (OB_FAIL(ddl_service.get_snapshot_mgr().batch_acquire_snapshot(
            ddl_service.get_sql_proxy(), SNAPSHOT_FOR_DDL, tenant_id_, schema_version_, snapshot_version, nullptr, tablet_ids))) {
      LOG_WARN("batch acquire snapshot failed", K(ret), K(tablet_ids));
    }
  }
  LOG_INFO("hold snapshot finished", K(ret), K(snapshot_version), K(object_id_), K(target_object_id_), K(schema_version_));
  return ret;
}

int ObDDLRedefinitionTask::release_snapshot(const int64_t snapshot_version)
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  ObSEArray<ObTabletID, 1> tablet_ids;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not init", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get tenant schema guard failed", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(object_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get table schema failed", K(ret), K(target_object_id_));
  } else if (OB_ISNULL(data_table_schema) || OB_ISNULL(dest_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table not exist", K(ret), K(object_id_), K(target_object_id_), KP(data_table_schema), KP(dest_table_schema));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, object_id_, tablet_ids))) {
    LOG_WARN("failed to get data table snapshot", K(ret));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, target_object_id_, tablet_ids))) {
    LOG_WARN("failed to get dest table snapshot", K(ret));
  } else if (data_table_schema->get_aux_lob_meta_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, data_table_schema->get_aux_lob_meta_tid(), tablet_ids))) {
    LOG_WARN("failed to get data lob meta table snapshot", K(ret));
  } else if (data_table_schema->get_aux_lob_piece_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, data_table_schema->get_aux_lob_piece_tid(), tablet_ids))) {
    LOG_WARN("failed to get data lob piece table snapshot", K(ret));
  } else if (dest_table_schema->get_aux_lob_meta_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, dest_table_schema->get_aux_lob_meta_tid(), tablet_ids))) {
    LOG_WARN("failed to get dest lob meta table snapshot", K(ret));
  } else if (dest_table_schema->get_aux_lob_piece_tid() != OB_INVALID_ID &&
             OB_FAIL(ObDDLUtil::get_tablets(tenant_id_, dest_table_schema->get_aux_lob_piece_tid(), tablet_ids))) {
    LOG_WARN("failed to get dest lob piece table snapshot", K(ret));
  } else if (OB_FAIL(batch_release_snapshot(snapshot_version, tablet_ids))) {
    LOG_WARN("failed to release snapshot", K(ret));
  }
  LOG_INFO("release snapshot finished", K(ret), K(snapshot_version), K(object_id_), K(target_object_id_), K(schema_version_));
  return ret;
}

// to hold snapshot, containing data in old table with new schema version.
int ObDDLRedefinitionTask::obtain_snapshot()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (snapshot_version_ > 0 && snapshot_held_) {
    // do nothing, already hold snapshot.
  } else if (!wait_trans_ctx_.is_inited()) {
    if (OB_FAIL(wait_trans_ctx_.init(tenant_id_, object_id_, ObDDLWaitTransEndCtx::WAIT_SCHEMA_TRANS, schema_version_))) {
      LOG_WARN("fail to init wait trans ctx", K(ret));
    }
  }
  // to get snapshot version.
  if (OB_SUCC(ret) && snapshot_version_ <= 0) {
    bool is_trans_end = false;
    const bool need_wait_trans_end = false;
    if (OB_FAIL(wait_trans_ctx_.try_wait(is_trans_end, snapshot_version_, need_wait_trans_end))) {
      LOG_WARN("just to get snapshot rather than wait trans end", K(ret));
    }
  }
  DEBUG_SYNC(DDL_REDEFINITION_HOLD_SNAPSHOT);
  // try hold snapshot
  if (OB_FAIL(ret)) {
  } else if (snapshot_version_ <= 0) {
    // the snapshot version obtained here must be valid.
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("snapshot version is invalid", K(ret), K(tenant_id_), K(object_id_), K(schema_version_));
  } else if (snapshot_version_ > 0 && !snapshot_held_) {
    if (OB_FAIL(ObDDLTaskRecordOperator::update_snapshot_version(root_service->get_sql_proxy(),
                                                                 tenant_id_,
                                                                 task_id_,
                                                                 snapshot_version_))) {
      LOG_WARN("update snapshot version failed", K(ret), K(task_id_));
    } else if (OB_FAIL(hold_snapshot(snapshot_version_))) {
      if (OB_SNAPSHOT_DISCARDED == ret) {
        snapshot_version_ = 0;
        snapshot_held_ = false;
        wait_trans_ctx_.reset();
      } else {
        LOG_WARN("hold snapshot version failed", K(ret));
      }
    } else {
      snapshot_held_ = true;
    }
  }
  return ret;
}

// list the column type modifications that support to validate the checksum.
bool ObDDLRedefinitionTask::check_can_validate_column_checksum(
    const bool is_oracle_mode,
    const ObColumnSchemaV2 &src_column_schema,
    const ObColumnSchemaV2 &dest_column_schema)
{
  bool can_validate_column_checksum = false;
  ObObjType src_column_type = src_column_schema.get_data_type();
  ObObjType dest_column_type = dest_column_schema.get_data_type();
  ObCollationType src_cs_type = src_column_schema.get_collation_type();
  ObCollationType dest_cs_type = dest_column_schema.get_collation_type();
  if (is_oracle_mode) {
    // to do, add more column types modification,
    if (ObCharType == src_column_type && ob_is_char(dest_column_type, dest_cs_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    } else if (ObVarcharType == src_column_type && ob_is_varchar(dest_column_type, dest_cs_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    } else if (ObNCharType == src_column_type && ob_is_nchar(dest_column_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    } else if (ObNVarchar2Type == src_column_type && ob_is_nvarchar2(dest_column_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    } else if (ObTimestampNanoType == src_column_type && ObTimestampNanoType == dest_column_type) {
      can_validate_column_checksum = true;
    } else if (ObURowIDType == src_column_type && ObURowIDType == dest_column_type) {
      can_validate_column_checksum = true;
    }
  } else {
    // to do, add more column types modification, bigint->int->mediumint->smallint for example.
    if (!src_column_schema.is_autoincrement() && dest_column_schema.is_autoincrement()) {
      // modify to auto increment can lead to data change, cannot verify checksum
      can_validate_column_checksum = false;
    } else if (ObIntTC == ob_obj_type_class(src_column_type) && ObIntTC == ob_obj_type_class(dest_column_type)) {
      can_validate_column_checksum = true;
    } else if (ObUIntTC == ob_obj_type_class(src_column_type) && ObUIntTC == ob_obj_type_class(dest_column_type)) {
      can_validate_column_checksum = true;
    } else if (ObMediumIntType == src_column_type && ObInt32Type == dest_column_type) {
      can_validate_column_checksum = true;
    } else if (ObCharType == src_column_type && ob_is_char(dest_column_type, dest_cs_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    } else if (ObVarcharType == src_column_type && ob_is_varchar(dest_column_type, dest_cs_type) && src_cs_type == dest_cs_type) {
      can_validate_column_checksum = true;
    }
  }
  return can_validate_column_checksum;
}

// Find the columns that need to validate the checksum, and put their id into validate_checksum_columns_id,
// which maps from old column id to new column id. Note that column ids are different in new hidden table.
int ObDDLRedefinitionTask::get_validate_checksum_columns_id(const ObTableSchema &data_table_schema,
  const ObTableSchema &dest_table_schema, hash::ObHashMap<uint64_t, uint64_t> &validate_checksum_columns_id)
{
  int ret = OB_SUCCESS;
  bool is_oracle_mode = false;
  if (OB_FAIL(alter_table_arg_.alter_table_schema_.check_if_oracle_compat_mode(is_oracle_mode))) {
    LOG_WARN("check if oracle mode failed", K(ret), K(object_id_), "dest_table_id", target_object_id_);
  } else {
    ObSQLMode sql_mode = alter_table_arg_.sql_mode_;
    if (is_oracle_mode) {
      sql_mode = SMO_STRICT_ALL_TABLES;
    } else {
      sql_mode = sql_mode & (~SMO_PAD_CHAR_TO_FULL_LENGTH);
    }
    ObArray<uint64_t> column_ids;
    ObColumnNameMap col_name_map;
    if (OB_FAIL(data_table_schema.get_column_ids(column_ids))) {
      LOG_WARN("get column ids failed", K(ret), K(object_id_));
    } else if (OB_FAIL(col_name_map.init(data_table_schema, alter_table_arg_.alter_table_schema_))) {
      LOG_WARN("failed to build column name map", K(ret));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      const uint64_t cur_column_id = column_ids.at(i);
      ObString dest_column_name;
      const ObColumnSchemaV2 *cur_column_schema = data_table_schema.get_column_schema(cur_column_id);
      const ObColumnSchemaV2 *dest_column_schema = NULL;
      if (OB_ISNULL(cur_column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("current column schema is null", K(ret), "data_table_id", object_id_, K(cur_column_id));
      } else if (OB_SUCCESS == (col_name_map.get(cur_column_schema->get_column_name_str(), dest_column_name))) {
        dest_column_schema = dest_table_schema.get_column_schema(dest_column_name);
      }
      if (OB_FAIL(ret)) {
      } else if (cur_column_schema->is_hidden_pk_column_id(cur_column_id) || cur_column_schema->is_generated_column()) {
        // do nothing, notice that the destination column schema of hidden pk is null while adding primary key for no primary key table;
      } else if (nullptr == dest_column_schema) {
        if (DDL_DROP_COLUMN == task_type_ || DDL_COLUMN_REDEFINITION == task_type_
            || DDL_TABLE_REDEFINITION == task_type_ || DDL_ALTER_PARTITION_BY == task_type_) {
          // column does not exist due to drop column op.
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("dest column schema is null", K(ret), K(task_type_), "dest_table_id", target_object_id_, "dest_column_id", cur_column_id);
        }
      } else if (is_lob_v2(dest_column_schema->get_data_type())) {
        // ignore to validate the checksum of the dest column that may be LOB.
        // the checksum of dest column is calculated based on the LOB index if the dest column is LOB.
      } else {
        // For modification on primary key, partiiton key, column type, ..., we should consider two cases of checksum validation.
        // 1. the dest column is same as src one(column type, len, precision, scale,...);
        // 2. strict mode and dest_column_type != src_column_type, check_can_validate_column_checksum(src_column_type, dest_column_type) = true;
        if (!(!cur_column_schema->is_autoincrement() && dest_column_schema->is_autoincrement())
          && cur_column_schema->get_data_type() == dest_column_schema->get_data_type()
          && cur_column_schema->get_data_length() == dest_column_schema->get_data_length()
          && cur_column_schema->get_data_precision() == dest_column_schema->get_data_precision()
          && cur_column_schema->get_data_scale() == dest_column_schema->get_data_scale()
          && cur_column_schema->get_encoding_type() == dest_column_schema->get_encoding_type()
          && cur_column_schema->get_collation_type() == dest_column_schema->get_collation_type()) {
          // some special cases that ignores to check, including:
          // 1. all set/enum type, but diffenent order of extended_type_info.
          if (ob_is_enum_or_set_type(cur_column_schema->get_data_type()) &&
            !is_array_equal(cur_column_schema->get_extended_type_info(), dest_column_schema->get_extended_type_info())) {
            // ignore to check the checksum.
          } else if (OB_FAIL(validate_checksum_columns_id.set_refactored(cur_column_id, dest_column_schema->get_column_id()))) {
            LOG_WARN("fail to append the column to validate the checksum", K(cur_column_id), K(ret));
          } else {
            LOG_INFO("succeed to append the column to validate the checksum", K(ret), K(cur_column_id));
          }
        } else if ((cur_column_schema->get_data_type() == dest_column_schema->get_data_type()) &&
          (cur_column_schema->get_encoding_type() != dest_column_schema->get_encoding_type() ||
          cur_column_schema->get_collation_type() != dest_column_schema->get_collation_type())) {
            // do not validate the column checksum if encoding type and collation type change;
        } else if (is_strict_mode(sql_mode) && check_can_validate_column_checksum(is_oracle_mode, *cur_column_schema, *dest_column_schema)) {
          if (OB_FAIL(validate_checksum_columns_id.set_refactored(cur_column_id, dest_column_schema->get_column_id()))) {
            LOG_WARN("fail to append the column to validate the checksum", K(ret), K(is_oracle_mode), K(is_strict_mode(sql_mode)),
            K(cur_column_schema->get_data_type()), K(dest_column_schema->get_data_type()),
            K(cur_column_schema->get_data_length()), K(dest_column_schema->get_data_length()));
          } else {
            LOG_INFO("succeed to append the column to validate the checksum", K(is_oracle_mode), K(is_strict_mode(sql_mode)),
            K(cur_column_schema->get_data_type()), K(dest_column_schema->get_data_type()),
            K(cur_column_schema->get_data_length()), K(dest_column_schema->get_data_length()));
          }
        } else {
            // do nothing, ignore to validate the checksum of this column.
        }
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::check_data_dest_tables_columns_checksum(const int64_t execution_id)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  hash::ObHashMap<uint64_t, uint64_t> validate_checksum_columns_id;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get schema guard failed", K(ret), K(tenant_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(object_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get dest table schema failed", K(ret), K(tenant_id_), K(target_object_id_));
  } else if (OB_ISNULL(data_table_schema) || OB_ISNULL(dest_table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_INFO("table is not exist", K(ret), K(object_id_), K(target_object_id_), KP(data_table_schema), KP(dest_table_schema));
  } else if (OB_FAIL(validate_checksum_columns_id.create(OB_MAX_COLUMN_NUMBER / 2, lib::ObLabel("DDLRedefTmp")))) {
    LOG_WARN("fail to create validate_checksum_columns_id set", K(ret));
  } else if (OB_FAIL(get_validate_checksum_columns_id(*data_table_schema, *dest_table_schema, validate_checksum_columns_id))) {
    LOG_WARN("fail to get columns id wvalidate the checksum", K(ret));
  } else {
    ObSqlString sql;
    hash::ObHashMap<int64_t, int64_t> data_table_column_checksums;
    hash::ObHashMap<int64_t, int64_t> dest_table_column_checksums;
    if (OB_FAIL(data_table_column_checksums.create(OB_MAX_COLUMN_NUMBER / 2, ObModIds::OB_CHECKSUM_CHECKER))) {
      LOG_WARN("fail to create datatable column checksum map", K(ret));
    } else if (OB_FAIL(dest_table_column_checksums.create(OB_MAX_COLUMN_NUMBER / 2, ObModIds::OB_CHECKSUM_CHECKER))) {
      LOG_WARN("fail to create desttable column checksum map", K(ret));
    } else if (OB_UNLIKELY(0 > execution_id || OB_INVALID_ID == object_id_ || !data_table_column_checksums.created())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret), K(execution_id), K(object_id_), K(data_table_column_checksums.created()));
    } else if (OB_UNLIKELY(OB_INVALID_ID == target_object_id_ || !dest_table_column_checksums.created())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid argument", K(ret),  "dest_table_id", target_object_id_, K(dest_table_column_checksums.created()));
    } else if (OB_FAIL(ObDDLChecksumOperator::get_table_column_checksum(tenant_id_, execution_id, object_id_, task_id_, data_table_column_checksums, GCTX.root_service_->get_sql_proxy()))) {
      LOG_WARN("fail to get table column checksum", K(ret), K(execution_id), "table_id", object_id_, K_(task_id), K(data_table_column_checksums.created()), KP(GCTX.root_service_));
    } else if (OB_FAIL(ObDDLChecksumOperator::get_table_column_checksum(tenant_id_, execution_id, target_object_id_, task_id_, dest_table_column_checksums, GCTX.root_service_->get_sql_proxy()))) {
      LOG_WARN("fail to get table column checksum", K(ret), K(execution_id), "table_id", target_object_id_, K_(task_id), K(dest_table_column_checksums.created()), KP(GCTX.root_service_));
    } else {
      uint64_t dest_column_id = 0;
      for (hash::ObHashMap<int64_t, int64_t>::const_iterator iter = data_table_column_checksums.begin();
          OB_SUCC(ret) && iter != data_table_column_checksums.end(); ++iter) {
        if (OB_FAIL(validate_checksum_columns_id.get_refactored(iter->first, dest_column_id))) {
          if (OB_HASH_NOT_EXIST != ret) {
            LOG_WARN("failed to get refactored", K(ret));
          } else {
            ret = OB_SUCCESS;
            LOG_INFO("ignore to validate the checksum of this column", "column_id", iter->first);
          }
        } else {
          int64_t dest_table_column_checksum = 0;
          if (OB_FAIL(dest_table_column_checksums.get_refactored(dest_column_id, dest_table_column_checksum))) {
            LOG_WARN("fail to get data table column checksum", K(ret), "column_id", iter->first,
            "column_name", data_table_schema->get_column_schema(iter->first)->get_column_name());
          } else if (dest_table_column_checksum == iter->second) {
            LOG_INFO("column checksum is equal", K(ret), "column_id", iter->first,  "column_name", data_table_schema->get_column_schema(iter->first)->get_column_name(),
            K(dest_table_column_checksum), "data_table_column_checksum", iter->second);
          } else {
            ret = OB_CHECKSUM_ERROR;
            LOG_WARN("column checksum is not equal", K(ret), K(object_id_), "dest_table_id", target_object_id_, "column_id", iter->first,
            "column_name", data_table_schema->get_column_schema(iter->first)->get_column_name(), K(dest_table_column_checksum), "data_table_column_checksum", iter->second);
          }
        }
      }
    }
    if (data_table_column_checksums.created()) {
      data_table_column_checksums.destroy();
    }
    if (dest_table_column_checksums.created()) {
      dest_table_column_checksums.destroy();
    }
  }
  if (validate_checksum_columns_id.created()) {
    validate_checksum_columns_id.destroy();
  }
  return ret;
}

int ObDDLRedefinitionTask::add_constraint_ddl_task(const int64_t constraint_id, ObSchemaGetterGuard &schema_guard)
{
  int ret = OB_SUCCESS;
  SMART_VAR(obrpc::ObAlterTableArg, alter_table_arg) {
    const ObTableSchema *table_schema = nullptr;
    AlterTableSchema &alter_table_schema = alter_table_arg.alter_table_schema_;
    const ObConstraint *constraint = nullptr;
    ObRootService *root_service = GCTX.root_service_;
    const ObDatabaseSchema *database_schema = nullptr;
    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
    } else if (OB_ISNULL(root_service)) {
      ret = OB_ERR_SYS;
      LOG_WARN("error sys, root service must not be nullptr", K(ret));
    } else if (OB_UNLIKELY(OB_INVALID_ID == constraint_id)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(ret), K(constraint_id));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, table_schema))) {
      LOG_WARN("get table schema failed", K(ret), K(tenant_id_));
    } else if (OB_ISNULL(table_schema)) {
      ret = OB_ERR_SYS;
      LOG_WARN("table schema must not be nullptr", K(ret));
    } else if (OB_FAIL(alter_table_schema.assign(*table_schema))) {
      LOG_WARN("assign table schema failed", K(ret));
    } else if (OB_ISNULL(constraint = table_schema->get_constraint(constraint_id))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get constraint failed", K(ret), K(constraint_id));
    } else if (OB_FAIL(schema_guard.get_database_schema(tenant_id_, table_schema->get_database_id(), database_schema))) {
      LOG_WARN("get database schema failed", K(ret), K_(tenant_id));
    } else if (OB_FAIL(alter_table_arg.tz_info_wrap_.deep_copy(alter_table_arg_.tz_info_wrap_))) {
      LOG_WARN("deep copy timezone info failed", K(ret));
    } else if (OB_FAIL(alter_table_arg.set_nls_formats(alter_table_arg_.nls_formats_))) {
      LOG_WARN("set nls formats failed", K(ret));
    } else {
      alter_table_arg.exec_tenant_id_ = tenant_id_;
      alter_table_arg.alter_constraint_type_ = obrpc::ObAlterTableArg::ADD_CONSTRAINT;
      alter_table_schema.clear_constraint();
      alter_table_schema.set_origin_database_name(database_schema->get_database_name_str());
      alter_table_schema.set_origin_table_name(table_schema->get_table_name_str());
      int64_t task_id = 0;
      if (OB_FAIL(alter_table_schema.add_constraint(*constraint))) {
        LOG_WARN("add constraint failed", K(ret));
      } else {
        const bool need_check = constraint->is_validated();
        if (need_check) {
          //TODO: shanting not null
          ObDDLTaskRecord task_record;
          ObCreateDDLTaskParam param(tenant_id_,
                                     ObDDLType::DDL_CHECK_CONSTRAINT,
                                     table_schema,
                                     nullptr,
                                     constraint_id,
                                     table_schema->get_schema_version(),
                                     0L/*parallelism*/,
                                     &allocator_,
                                     &alter_table_arg,
                                     task_id_);
          if (OB_FAIL(root_service->get_ddl_task_scheduler().create_ddl_task(param,
                                                                             *GCTX.sql_proxy_,
                                                                             task_record))) {
            if (OB_ENTRY_EXIST == ret) {
              ret = OB_SUCCESS;
            } else {
              LOG_WARN("submit ddl task failed", K(ret));
            }
          } else if (OB_FAIL(root_service->get_ddl_task_scheduler().schedule_ddl_task(task_record))) {
            LOG_WARN("fail to schedule ddl task", K(ret), K(task_record));
          } else {
            task_id = task_record.task_id_;
          }
          if (OB_SUCC(ret)) {
            DependTaskStatus status;
            bool need_set_status = false;
            ObDDLTaskKey task_key(constraint_id, table_schema->get_schema_version());
            status.task_id_ = task_id; // child task id, which is used to judge child task finish.
            if (OB_FAIL(dependent_task_result_map_.get_refactored(task_key, status))) {
              if (OB_HASH_NOT_EXIST != ret) {
                LOG_WARN("get from dependent task map failed", K(ret));
              } else {
                ret = OB_SUCCESS;
                need_set_status = true;
              }
            }
            if (OB_SUCC(ret) && need_set_status) {
              status.task_id_ = task_id; // child task id is used to judge whether child task finish.
              if (OB_FAIL(dependent_task_result_map_.set_refactored(task_key, status))) {
                LOG_WARN("set dependent task map failed", K(ret), K(task_key));
              } else {
                LOG_INFO("add constraint task", K(task_key));
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::add_fk_ddl_task(const int64_t fk_id, ObSchemaGetterGuard &schema_guard)
{
  int ret = OB_SUCCESS;
  const ObTableSchema *orig_table_schema = nullptr;
  const ObTableSchema *hidden_table_schema = nullptr;
  SMART_VAR(obrpc::ObAlterTableArg, alter_table_arg) {
    AlterTableSchema &alter_table_schema = alter_table_arg.alter_table_schema_;
    ObConstraint *constraint = nullptr;
    ObRootService *root_service = GCTX.root_service_;
    const ObDatabaseSchema *database_schema = nullptr;
    if (OB_UNLIKELY(!is_inited_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
    } else if (OB_ISNULL(root_service)) {
      ret = OB_ERR_SYS;
      LOG_WARN("error sys, root service must not be nullptr", K(ret));
    } else if (OB_UNLIKELY(OB_INVALID_ID == fk_id)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid arguments", K(ret), K(fk_id));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, orig_table_schema))) {
      LOG_WARN("get table schema failed", K(ret), K(tenant_id_));
    } else if (OB_ISNULL(orig_table_schema)) {
      ret = OB_ERR_SYS;
      LOG_WARN("error sys, table schema must not be nullptr", K(ret));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, hidden_table_schema))) {
      LOG_WARN("get table schema failed", K(ret), K(tenant_id_));
    } else if (OB_ISNULL(hidden_table_schema)) {
      ret = OB_ERR_SYS;
      LOG_WARN("error sys, table schema must not be nullptr", K(ret));
    } else if (OB_FAIL(alter_table_schema.assign(*hidden_table_schema))) {
      LOG_WARN("assign table schema failed", K(ret));
    } else if (OB_FAIL(schema_guard.get_database_schema(tenant_id_, orig_table_schema->get_database_id(), database_schema))) {
      LOG_WARN("get database schema failed", K(ret), K_(tenant_id));
    } else if (OB_FAIL(alter_table_arg.tz_info_wrap_.deep_copy(alter_table_arg_.tz_info_wrap_))) {
      LOG_WARN("deep copy timezone info failed", K(ret));
    } else if (OB_FAIL(alter_table_arg.set_nls_formats(alter_table_arg_.nls_formats_))) {
      LOG_WARN("set nls formats failed", K(ret));
    } else {
      obrpc::ObCreateForeignKeyArg fk_arg;
      ObForeignKeyInfo fk_info;
      bool found = false;
      const common::ObIArray<ObForeignKeyInfo> &fk_info_array = hidden_table_schema->get_foreign_key_infos();
      alter_table_schema.set_origin_database_name(database_schema->get_database_name_str());
      alter_table_schema.set_origin_table_name(orig_table_schema->get_table_name_str());
      alter_table_arg.table_id_ = object_id_;
      alter_table_arg.hidden_table_id_ = target_object_id_;
      alter_table_arg.exec_tenant_id_ = tenant_id_;
      for (int64_t i = 0; OB_SUCC(ret) && i < fk_info_array.count(); ++i) {
        const ObForeignKeyInfo &tmp_fk_info	= fk_info_array.at(i);
        if (tmp_fk_info.foreign_key_id_ == fk_id) {
          fk_info = tmp_fk_info;
          found = true;
          break;
        }
      }
      if (OB_SUCC(ret)) {
        if (!found) {
          ret = OB_ENTRY_NOT_EXIST;
          LOG_WARN("cannot find foreign key in table", K(ret), K(fk_id), K(fk_info_array));
        } else {
          DependTaskStatus status;
          ObDDLTaskKey task_key(fk_id, hidden_table_schema->get_schema_version());
          fk_arg.foreign_key_name_ = fk_info.foreign_key_name_;
          fk_arg.enable_flag_ = fk_info.enable_flag_;
          fk_arg.is_modify_enable_flag_ = fk_info.enable_flag_;
          fk_arg.ref_cst_type_ = fk_info.ref_cst_type_;
          fk_arg.ref_cst_id_ = fk_info.ref_cst_id_;
          fk_arg.validate_flag_ = fk_info.validate_flag_;
          fk_arg.is_modify_validate_flag_ = fk_info.validate_flag_;
          fk_arg.rely_flag_ = fk_info.rely_flag_;
          fk_arg.is_modify_rely_flag_ = fk_info.is_modify_rely_flag_;
          fk_arg.is_modify_fk_state_ = fk_info.is_modify_fk_state_;
          fk_arg.need_validate_data_ = fk_info.validate_flag_;
          ObDDLTaskRecord task_record;
          ObCreateDDLTaskParam param(tenant_id_,
                                     ObDDLType::DDL_FOREIGN_KEY_CONSTRAINT,
                                     hidden_table_schema,
                                     nullptr,
                                     fk_id,
                                     hidden_table_schema->get_schema_version(),
                                     0L/*parallelism*/,
                                     &allocator_,
                                     &alter_table_arg,
                                     task_id_);
          if (OB_FAIL(alter_table_arg.foreign_key_arg_list_.push_back(fk_arg))) {
            LOG_WARN("push back foreign key arg failed", K(ret));
          } else if (OB_FAIL(root_service->get_ddl_task_scheduler().create_ddl_task(param, *GCTX.sql_proxy_, task_record))) {
            LOG_WARN("submit ddl task failed", K(ret));
          } else if (OB_FAIL(root_service->get_ddl_task_scheduler().schedule_ddl_task(task_record))) {
            LOG_WARN("fail to schedule ddl task", K(ret), K(task_record));
          } else if (FALSE_IT(status.task_id_ = task_record.task_id_)) { // child task id is used to judge whether child task finish.
          } else if (OB_FAIL(dependent_task_result_map_.set_refactored(task_key, status))) {
            LOG_WARN("set dependent task map failed", K(ret));
          } else {
            LOG_INFO("add foregin key ddl task", K(fk_arg), "ddl_task_key", task_key);
          }
        }
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::on_child_task_finish(
    const ObDDLTaskKey &child_task_key,
    const int ret_code)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_UNLIKELY(!child_task_key.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), K(child_task_key));
  } else {
    TCWLockGuard guard(lock_);
    int64_t org_ret = INT64_MAX;
    DependTaskStatus status;
    if (OB_FAIL(dependent_task_result_map_.get_refactored(child_task_key, status))) {
      if (OB_HASH_NOT_EXIST == ret) {
        ret = OB_ENTRY_NOT_EXIST;
      }
      LOG_WARN("get from dependent_task_result_map failed", K(ret), K(child_task_key));
    } else if (org_ret != INT64_MAX && org_ret != ret_code) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("error unexpected, ddl result triggers twice", K(ret), K(child_task_key));
    } else if (FALSE_IT(status.ret_code_ = ret_code)) {
    } else if (OB_FAIL(dependent_task_result_map_.set_refactored(child_task_key, status, true/*overwrite*/))) {
      LOG_WARN("set dependent_task_result_map failed", K(ret), K(child_task_key));
    } else {
      LOG_INFO("child task finish successfully", K(child_task_key));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_auto_increment_position()
{
  int ret = OB_SUCCESS;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (has_synced_autoincrement_) {
    // do nothing
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get schema guard failed", K(ret), K(tenant_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(object_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get dest table schema failed", K(ret), K(tenant_id_), K(target_object_id_));
  } else {
    ObArray<uint64_t> column_ids;
    if (OB_FAIL(data_table_schema->get_column_ids(column_ids))) {
      LOG_WARN("get column ids failed", K(ret), K(object_id_));
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
      uint64_t cur_column_id = column_ids.at(i);
      const ObColumnSchemaV2 *cur_column_schema = data_table_schema->get_column_schema(cur_column_id);
      const ObColumnSchemaV2 *dst_column_schema = NULL;
      if (OB_ISNULL(cur_column_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("current column schema is null", K(ret), K(target_object_id_), K(cur_column_id));
      } else if (cur_column_schema->is_autoincrement()
      && nullptr != (dst_column_schema = dest_table_schema->get_column_schema(cur_column_schema->get_column_name()))
      && dst_column_schema->is_autoincrement()) {
        // Worker timeout ts here is default value, i.e., INT64_MAX,
        // which leads to RPC-receiver worker timeout due to overflow when select val from __ALL_AUTO_INCREMENT.
        // More details, refer to comments in https://work.aone.alibaba-inc.com/issue/42761282.
        const int64_t save_timeout_ts = THIS_WORKER.get_timeout_ts();
        THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + max(GCONF.rpc_timeout, 1000 * 1000 * 20L));
        ObAutoincrementService &auto_inc_service = ObAutoincrementService::get_instance();
        uint64_t sequence_value = 0;
        AutoincParam param;
        param.tenant_id_ = tenant_id_;
        param.autoinc_table_id_ = target_object_id_;
        param.autoinc_first_part_num_ = dest_table_schema->get_first_part_num();
        param.autoinc_table_part_num_ = dest_table_schema->get_all_part_num();
        param.autoinc_col_id_ = dst_column_schema->get_column_id();
        param.part_level_ = dest_table_schema->get_part_level();
        ObObjType column_type = dst_column_schema->get_data_type();
        param.autoinc_col_type_ = column_type;
        param.autoinc_desired_count_ = 0;
        param.autoinc_increment_ = 1;
        param.autoinc_offset_ = 1;
        param.auto_increment_cache_size_ = 1; // TODO(shuangcan): should we use the sysvar on session?
        param.autoinc_mode_is_order_ = dest_table_schema->is_order_auto_increment_mode();
        if (OB_FAIL(auto_inc_service.get_sequence_value(tenant_id_, object_id_, cur_column_id, param.autoinc_mode_is_order_, sequence_value))) {
          LOG_WARN("get sequence value failed", K(ret), K(tenant_id_), K(object_id_), K(cur_column_id));
        } else if (FALSE_IT(param.global_value_to_sync_ = sequence_value - 1)) {
          // as sequence_value is an avaliable value. sync value will not be avaliable to user
        } else if (OB_FAIL(auto_inc_service.sync_insert_value_global(param))) {
          LOG_WARN("set auto increment position failed", K(ret), K(tenant_id_), K(target_object_id_), K(cur_column_id), K(param));
        } else {
          has_synced_autoincrement_ = true;
          LOG_INFO("sync auto increment position succ", K(ret), K(sequence_value), K(object_id_),
          K(target_object_id_), K(dst_column_schema->get_column_id()));
        }
        THIS_WORKER.set_timeout_ts(save_timeout_ts);
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::unlock_table()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  const ObTableSchema *dest_table_schema = nullptr;
  ObMultiVersionSchemaService &schema_service = ObMultiVersionSchemaService::get_instance();
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get tenant schema failed", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get table schema failed", K(ret));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, dest_table_schema))) {
    LOG_WARN("get table schema failed", K(ret));
  } 
  
  // In scenario like succeed to cleanup garbage but RPC timeout occurs, executing the function again
  // will find data/dest table not exist.
  if (OB_FAIL(ret)) {
  } else if (nullptr == data_table_schema || data_table_schema->is_tmp_table()) {
  } else if (OB_FAIL(ObTableLockRpcClient::get_instance().unlock_table(object_id_,
                                      EXCLUSIVE, schema_version_, 0, tenant_id_))) {
    if (OB_OBJ_LOCK_NOT_EXIST == ret || OB_TABLE_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("unlock source table failed", K(ret), K(object_id_));
    }
  }

  if (OB_FAIL(ret)) {
  } else if (nullptr == dest_table_schema || dest_table_schema->is_tmp_table()) {
  } else if (OB_FAIL(ObTableLockRpcClient::get_instance().unlock_table(target_object_id_,
                                      EXCLUSIVE, schema_version_, 0, tenant_id_))) {
    if (OB_OBJ_LOCK_NOT_EXIST == ret || OB_TABLE_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
    } else {
      LOG_WARN("unlock dest table failed", K(ret), K(target_object_id_));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::modify_autoinc(const ObDDLTaskStatus next_task_status)
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  bool is_update_autoinc_end = false;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(check_update_autoinc_end(is_update_autoinc_end))) {
    LOG_WARN("update autoinc failed", K(ret));
  } else {
    ObDDLService &ddl_service = root_service->get_ddl_service();
    ObMultiVersionSchemaService &schema_service = ddl_service.get_schema_service();
    ObMySQLProxy &sql_proxy = ddl_service.get_sql_proxy();
    const ObTableSchema *orig_table_schema = nullptr;
    ObSchemaGetterGuard schema_guard;
    AlterTableSchema &alter_table_schema = alter_table_arg_.alter_table_schema_;
    const ObTableSchema *new_table_schema = nullptr;
    uint64_t alter_autoinc_column_id = 0;
    ObColumnNameMap col_name_map;
    if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
      LOG_WARN("get schema guard failed", K(ret), K(tenant_id_));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, orig_table_schema))) {
      LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(object_id_));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, new_table_schema))) {
      LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(target_object_id_));
    } else if (OB_ISNULL(orig_table_schema) || OB_ISNULL(new_table_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("table schemas should not be null", K(ret), K(orig_table_schema), K(new_table_schema));
    } else if (OB_FAIL(col_name_map.init(*orig_table_schema, alter_table_schema))) {
      LOG_WARN("failed to init column name map", K(ret));
    } else if (!is_update_autoinc_end && update_autoinc_job_time_ == 0) {
      ObTableSchema::const_column_iterator iter = alter_table_schema.column_begin();
      ObTableSchema::const_column_iterator iter_end = alter_table_schema.column_end();
      AlterColumnSchema *alter_column_schema = nullptr;
      for(; OB_SUCC(ret) && iter != iter_end; iter++) {
        if (OB_ISNULL(alter_column_schema = static_cast<AlterColumnSchema *>(*iter))) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("iter is NULL", K(ret));
        } else {
          const ObString &orig_column_name = alter_column_schema->get_origin_column_name();
          if (!orig_column_name.empty()) {
            const ObColumnSchemaV2 *orig_column_schema = orig_table_schema->get_column_schema(orig_column_name);
            if (OB_ISNULL(orig_column_schema)) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("orig column schema is null", K(ret));
            } else if (alter_column_schema->is_autoincrement() && !orig_column_schema->is_autoincrement()) {
              ObString new_column_name;
              const ObColumnSchemaV2 *new_column_schema = nullptr;
              if (OB_FAIL(col_name_map.get(orig_column_name, new_column_name))) {
                LOG_WARN("invalid orig column name", K(ret), K(orig_table_schema), K(alter_table_schema));
              } else if (OB_ISNULL(new_column_schema = new_table_schema->get_column_schema(new_column_name))) {
                ret = OB_ERR_UNEXPECTED;
                LOG_WARN("new column schema is null", K(ret), K(new_column_name), K(new_table_schema));
              } else {
                alter_autoinc_column_id = new_column_schema->get_column_id();
                alter_table_schema.set_autoinc_column_id(alter_autoinc_column_id);
                break; // there can only be one autoinc column
              }
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        if (alter_autoinc_column_id != 0) { // there is an autoinc column
          ObObjType column_type = new_table_schema->get_column_schema(alter_autoinc_column_id)->get_data_type();
          ObUpdateAutoincSequenceTask task(tenant_id_, object_id_, target_object_id_, schema_version_,
                                          alter_autoinc_column_id, column_type, alter_table_arg_.sql_mode_,
                                          trace_id_, task_id_);
          if (OB_FAIL(root_service->submit_ddl_single_replica_build_task(task))) {
            LOG_WARN("fail to submit ObUpdateAutoincSequenceTask", K(ret));
          } else {
            update_autoinc_job_time_ = ObTimeUtility::current_time();
            LOG_INFO("submit ObUpdateAutoincSequenceTask success", K(object_id_), K(alter_autoinc_column_id));
          }
        } else {
          // no autoinc modify
          is_update_autoinc_end = true;
        }
      }
    }

    alter_autoinc_column_id = alter_table_schema.get_autoinc_column_id();
    if (OB_SUCC(ret) && is_update_autoinc_end && alter_autoinc_column_id != 0
        && OB_NOT_NULL(new_table_schema)) {
      const int64_t save_timeout_ts = THIS_WORKER.get_timeout_ts();
      THIS_WORKER.set_timeout_ts(ObTimeUtility::current_time() + max(GCONF.rpc_timeout, 1000 * 1000 * 20L));
      ObAutoincrementService &auto_inc_service = ObAutoincrementService::get_instance();
      ObDDLService &ddl_service = root_service->get_ddl_service();
      ObMultiVersionSchemaService &schema_service = ddl_service.get_schema_service();
      ObMySQLProxy &sql_proxy = ddl_service.get_sql_proxy();
      const uint64_t autoinc_val = alter_table_schema.get_auto_increment();
      AutoincParam param;
      param.tenant_id_ = tenant_id_;
      param.autoinc_table_id_ = target_object_id_;
      param.autoinc_first_part_num_ = new_table_schema->get_first_part_num();
      param.autoinc_table_part_num_ = new_table_schema->get_all_part_num();
      param.autoinc_col_id_ = alter_autoinc_column_id;
      param.part_level_ = new_table_schema->get_part_level();
      ObObjType column_type = new_table_schema->get_column_schema(alter_autoinc_column_id)->get_data_type();
      param.autoinc_col_type_ = column_type;
      param.autoinc_desired_count_ = 0;
      param.autoinc_increment_ = 1;
      param.autoinc_offset_ = 1;
      param.global_value_to_sync_ = autoinc_val - 1;
      param.auto_increment_cache_size_ = 1; // TODO(shuangcan): should we use the sysvar on session?
      if (OB_FAIL(auto_inc_service.sync_insert_value_global(param))) {
        LOG_WARN("fail to clear autoinc cache", K(ret), K(param));
      }
      THIS_WORKER.set_timeout_ts(save_timeout_ts);
    }
  }

  if (OB_FAIL(ret) || is_update_autoinc_end) {
    if (OB_FAIL(switch_status(next_task_status, ret))) {
      LOG_WARN("fail to switch status", K(ret));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::cleanup()
{
  int ret = OB_SUCCESS;
  ObString unused_str;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(report_error_code(unused_str))) {
    LOG_WARN("report error code failed", K(ret));
  } else if (OB_FAIL(remove_task_record())) {
    LOG_WARN("remove task record failed", K(ret));
  }
  return ret;
}

int ObDDLRedefinitionTask::finish()
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard schema_guard;
  const ObTableSchema *data_table_schema = nullptr;
  ObSArray<uint64_t> objs;
  alter_table_arg_.ddl_task_type_ = share::CLEANUP_GARBAGE_TASK;
  alter_table_arg_.table_id_ = object_id_;
  alter_table_arg_.hidden_table_id_ = target_object_id_;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (OB_FAIL(unlock_table())) {
    LOG_WARN("unlock table failed", K(ret));
  } else if (snapshot_version_ > 0 && OB_FAIL(release_snapshot(snapshot_version_))) {
    LOG_WARN("release snapshot failed", K(ret));
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance().get_tenant_schema_guard(tenant_id_, schema_guard))) {
    LOG_WARN("get schema guard failed", K(ret), K(tenant_id_));
  } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
    LOG_WARN("get data table schema failed", K(ret), K(tenant_id_), K(object_id_));
  } else if (nullptr != data_table_schema && data_table_schema->get_association_table_id() != OB_INVALID_ID &&
            OB_FAIL(root_service->get_ddl_service().get_common_rpc()->to(obrpc::ObRpcProxy::myaddr_).
                execute_ddl_task(alter_table_arg_, objs))) {
    LOG_WARN("cleanup garbage failed", K(ret));
  } else if (OB_FAIL(cleanup())) {
    LOG_WARN("clean up failed", K(ret));
  }
  return ret;
}

int ObDDLRedefinitionTask::fail()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(finish())) {
    LOG_WARN("finish failed", K(ret));
  } else {
    need_retry_ = false;
  }
  return ret;
}

int ObDDLRedefinitionTask::success()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObDDLRedefinitionTask has not been inited", K(ret));
  } else if (OB_FAIL(finish())) {
    LOG_WARN("finish failed", K(ret));
  } else {
    need_retry_ = false;
  }
  return ret;
}

int ObDDLRedefinitionTask::check_update_autoinc_end(bool &is_end)
{
  int ret = OB_SUCCESS;
  if (INT64_MAX == update_autoinc_job_ret_code_) {
    // not finish
  } else {
    is_end = true;
    ret_code_ = update_autoinc_job_ret_code_;
    ret = ret_code_;
  }
  return ret;
}

int ObDDLRedefinitionTask::check_check_table_empty_end(bool &is_end)
{
  int ret = OB_SUCCESS;
  if (INT64_MAX == check_table_empty_job_ret_code_) {
    // not finish
  } else {
    is_end = true;
    ret_code_ = check_table_empty_job_ret_code_;
    ret = ret_code_;
  }
  return ret;
}

int ObDDLRedefinitionTask::notify_update_autoinc_finish(const uint64_t autoinc_val, const int ret_code)
{
  int ret = OB_SUCCESS;
  update_autoinc_job_ret_code_ = ret_code;
  alter_table_arg_.alter_table_schema_.set_auto_increment(autoinc_val);
  return ret;
}

int ObDDLRedefinitionTask::serialize_params_to_message(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(nullptr == buf || buf_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(buf), K(buf_len));
  } else if (OB_FAIL(serialization::encode_i64(buf, buf_len, pos, task_version_))) {
    LOG_WARN("fail to serialize task version", K(ret), K(task_version_));
  } else if (OB_FAIL(alter_table_arg_.serialize(buf, buf_len, pos))) {
    LOG_WARN("serialize table arg failed", K(ret));
  } else {
    LST_DO_CODE(OB_UNIS_ENCODE, parallelism_);
  }
  return ret;
}

int ObDDLRedefinitionTask::deserlize_params_from_message(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  obrpc::ObAlterTableArg tmp_arg;
  if (OB_UNLIKELY(nullptr == buf || data_len <= 0)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(buf), K(data_len));
  } else if (OB_FAIL(serialization::decode_i64(buf, data_len, pos, &task_version_))) {
    LOG_WARN("fail to deserialize task version", K(ret));
  } else if (OB_FAIL(tmp_arg.deserialize(buf, data_len, pos))) {
    LOG_WARN("serialize table failed", K(ret));
  } else if (OB_FAIL(deep_copy_table_arg(allocator_, tmp_arg, alter_table_arg_))) {
    LOG_WARN("deep copy table arg failed", K(ret));
  } else {
    LST_DO_CODE(OB_UNIS_DECODE, parallelism_);
  }
  return ret;
}

int64_t ObDDLRedefinitionTask::get_serialize_param_size() const
{
  return alter_table_arg_.get_serialize_size() + serialization::encoded_length_i64(task_version_)
         + serialization::encoded_length_i64(parallelism_);
}

int ObDDLRedefinitionTask::check_health()
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("not inited", K(ret));
  } else if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys", K(ret));
  } else if (!root_service->in_service()) {
    ret = OB_STATE_NOT_MATCH;
    LOG_WARN("root service not in service, do not need retry", K(ret), K(object_id_), K(target_object_id_));
    need_retry_ = false;
  } else if (OB_FAIL(refresh_status())) { // refresh task status
    LOG_WARN("refresh status failed", K(ret));
  } else if (OB_FAIL(refresh_schema_version())) {
    LOG_WARN("refresh schema version failed", K(ret));
  } else {
    ObMultiVersionSchemaService &schema_service = root_service->get_schema_service();
    ObSchemaGetterGuard schema_guard;
    const ObTableSchema *index_schema = nullptr;
    bool is_source_table_exist = false;
    bool is_dest_table_exist = false;
    if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
      LOG_WARN("get tanant schema guard failed", K(ret), K(tenant_id_));
    } else if (OB_FAIL(schema_guard.check_table_exist(tenant_id_, object_id_, is_source_table_exist))) {
      LOG_WARN("check data table exist failed", K(ret), K_(tenant_id), K(object_id_));
    } else if (OB_FAIL(schema_guard.check_table_exist(tenant_id_, target_object_id_, is_dest_table_exist))) {
      LOG_WARN("check index table exist failed", K(ret), K_(tenant_id), K(is_dest_table_exist));
    } else if (!is_source_table_exist || !is_dest_table_exist) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("data table or dest table not exist", K(ret), K(is_source_table_exist), K(is_dest_table_exist));
    }
    if (OB_FAIL(ret) && !ObIDDLTask::in_ddl_retry_white_list(ret)) {
      const ObDDLTaskStatus old_status = static_cast<ObDDLTaskStatus>(task_status_);
      const ObDDLTaskStatus new_status = ObDDLTaskStatus::FAIL;
      switch_status(new_status, ret);
      LOG_WARN("switch status to build_failed", K(ret), K(old_status), K(new_status));
    }
  }
  if (ObDDLTaskStatus::FAIL == static_cast<ObDDLTaskStatus>(task_status_)
      || ObDDLTaskStatus::SUCCESS == static_cast<ObDDLTaskStatus>(task_status_)) {
    ret = OB_SUCCESS; // allow clean up
  }
  return ret;
}

int ObDDLRedefinitionTask::get_estimated_timeout(const ObTableSchema *dst_table_schema, int64_t &estimated_timeout)
{
  int ret = OB_SUCCESS;
  ObArray<ObTabletID> tablet_ids;
  ObArray<ObObjectID> partition_ids;
  if (OB_ISNULL(dst_table_schema)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(dst_table_schema));
  } else if (OB_FAIL(dst_table_schema->get_all_tablet_and_object_ids(tablet_ids, partition_ids))) {
    LOG_WARN("get all tablet and object ids failed", K(ret));
  } else {
    estimated_timeout = tablet_ids.count() * dst_table_schema->get_column_count() * 1000L; // 1ms for each column
    estimated_timeout = max(estimated_timeout, 9 * 1000 * 1000);
    estimated_timeout = min(estimated_timeout, 3600 * 1000 * 1000);
    estimated_timeout = max(estimated_timeout, GCONF.rpc_timeout);
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_stats_info()
{
  int ret = OB_SUCCESS;
  ObRootService *root_service = GCTX.root_service_;
  if (OB_ISNULL(root_service)) {
    ret = OB_ERR_SYS;
    LOG_WARN("error sys, root service must not be nullptr", K(ret));
  } else if (has_synced_stats_info_) {
  } else {
    ObMultiVersionSchemaService &schema_service = root_service->get_schema_service();
    ObMySQLTransaction trans;
    ObSchemaGetterGuard schema_guard;
    const ObTableSchema *data_table_schema = nullptr;
    const ObTableSchema *new_table_schema = nullptr;
    ObTimeoutCtx timeout_ctx;
    int64_t timeout = 0;

    if (OB_FAIL(schema_service.get_tenant_schema_guard(tenant_id_, schema_guard))) {
      LOG_WARN("get tanant schema guard failed", K(ret), K(tenant_id_));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, object_id_, data_table_schema))) {
      LOG_WARN("fail to get data table schema", K(ret), K(object_id_));
    } else if (OB_FAIL(schema_guard.get_table_schema(tenant_id_, target_object_id_, new_table_schema))) {
      LOG_WARN("fail to get data table schema", K(ret), K(target_object_id_));
    } else if (OB_ISNULL(data_table_schema) || OB_ISNULL(new_table_schema)) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table schema is null", K(ret));
    } else if (OB_FAIL(get_estimated_timeout(new_table_schema, timeout))) {
      LOG_WARN("get estimated timeout failed", K(ret));
    } else if (OB_FAIL(timeout_ctx.set_trx_timeout_us(timeout))) {
      LOG_WARN("set timeout ctx failed", K(ret));
    } else if (OB_FAIL(timeout_ctx.set_timeout(timeout))) {
      LOG_WARN("set timeout failed", K(ret));
    } else if (OB_FAIL(trans.start(&root_service->get_sql_proxy(), tenant_id_))) {
      LOG_WARN("fail to start transaction", K(ret));
    } else if (OB_FAIL(sync_table_level_stats_info(trans, *data_table_schema))) {
      LOG_WARN("fail to sync table level stats", K(ret));
    } else if (DDL_ALTER_PARTITION_BY != task_type_
               && OB_FAIL(sync_partition_level_stats_info(trans, *data_table_schema, *new_table_schema))) {
      LOG_WARN("fail to sync partition level stats", K(ret));
    } else if (OB_FAIL(sync_column_level_stats_info(trans, *data_table_schema, *new_table_schema, schema_guard))) {
      LOG_WARN("fail to sync column level stats", K(ret));
    }

    if (trans.is_started()) {
      bool is_commit = (ret == OB_SUCCESS);
      int tmp_ret = trans.end(is_commit);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("fail to end trans", K(ret), K(is_commit));
        if (OB_SUCC(ret)) {
          ret = tmp_ret;
        }
      } else {
        has_synced_stats_info_ = (ret == OB_SUCCESS);
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_table_level_stats_info(common::ObMySQLTransaction &trans,
                                                       const ObTableSchema &data_table_schema)
{
  int ret = OB_SUCCESS;
  ObSqlString sql_string;
  ObSqlString history_sql_string;
  int64_t affected_rows = 0;
  // for partitioned table, table-level stat is -1, for non-partitioned table, table-level stat is table id
  int64_t partition_id = -1;
  int64_t target_partition_id = -1;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (!data_table_schema.is_partitioned_table()) {
    partition_id = object_id_;
    target_partition_id = target_object_id_;
  }
  if (OB_FAIL(sql_string.assign_fmt("UPDATE %s SET table_id = %ld, partition_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld",
      OB_ALL_TABLE_STAT_TNAME, target_object_id_, target_partition_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id))) {
    LOG_WARN("fail to assign sql string", K(ret));
  } else if (OB_FAIL(history_sql_string.assign_fmt("UPDATE %s SET table_id = %ld, partition_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld",
      OB_ALL_TABLE_STAT_HISTORY_TNAME, target_object_id_, target_partition_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id))) {
    LOG_WARN("fail to assign history sql string", K(ret));
  } else if (OB_FAIL(trans.write(tenant_id_, sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_table_stat", K(ret), K(sql_string));
  } else if (OB_UNLIKELY(affected_rows < 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
  } else if (OB_FAIL(trans.write(tenant_id_, history_sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_table_stat_history", K(ret), K(sql_string));
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_partition_level_stats_info(common::ObMySQLTransaction &trans,
                                                           const ObTableSchema &data_table_schema,
                                                           const ObTableSchema &new_table_schema)
{
  int ret = OB_SUCCESS;
  ObArray<ObObjectID> src_partition_ids;
  ObArray<ObObjectID> dest_partition_ids;
  ObArray<ObTabletID> src_tablet_ids;
  ObArray<ObTabletID> dest_tablet_ids;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (!data_table_schema.is_partitioned_table()) {
    // if not partition table, no need to sync partition level stats
  } else if (OB_FAIL(data_table_schema.get_all_tablet_and_object_ids(src_tablet_ids, src_partition_ids))) {
    LOG_WARN("fail to get all tablet and object ids", K(ret));
  } else if (OB_FAIL(new_table_schema.get_all_tablet_and_object_ids(dest_tablet_ids, dest_partition_ids))) {
    LOG_WARN("fail to get all tablet and object ids", K(ret));
  } else {
    const int64_t BATCH_SIZE = 256;
    int64_t batch_start = 0;
    while (OB_SUCC(ret) && batch_start < src_partition_ids.count()) {
      ObSqlString sql_string;
      ObSqlString history_sql_string;
      int64_t affected_rows = 0;
      const int64_t batch_end = std::min(src_partition_ids.count(), batch_start + BATCH_SIZE) - 1;
      if (OB_FAIL(generate_sync_partition_level_stats_sql(OB_ALL_TABLE_STAT_TNAME,
                                                          src_partition_ids,
                                                          dest_partition_ids,
                                                          batch_start,
                                                          batch_end,
                                                          sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(generate_sync_partition_level_stats_sql(OB_ALL_TABLE_STAT_HISTORY_TNAME,
                                                                 src_partition_ids,
                                                                 dest_partition_ids,
                                                                 batch_start,
                                                                 batch_end,
                                                                 history_sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(trans.write(tenant_id_, sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_table_stat", K(ret), K(sql_string));
      } else if (OB_UNLIKELY(affected_rows < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
      } else if (OB_FAIL(trans.write(tenant_id_, history_sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_table_stat_history", K(ret), K(sql_string));
      } else if (OB_UNLIKELY(affected_rows < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected affected_rows", K(ret), K(affected_rows));
      } else {
        batch_start += BATCH_SIZE;
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_column_level_stats_info(common::ObMySQLTransaction &trans,
                                                        const ObTableSchema &data_table_schema,
                                                        const ObTableSchema &new_table_schema,
                                                        ObSchemaGetterGuard &schema_guard)
{
  int ret = OB_SUCCESS;
  AlterTableSchema &alter_table_schema = alter_table_arg_.alter_table_schema_;
  ObColumnNameMap col_name_map;
  if (OB_FAIL(col_name_map.init(data_table_schema, alter_table_schema))) {
    LOG_WARN("failed to init column name map", K(ret));
  } else {
    ObTableSchema::const_column_iterator iter = data_table_schema.column_begin();
    ObTableSchema::const_column_iterator iter_end = data_table_schema.column_end();
    for (; OB_SUCC(ret) && iter != iter_end; iter++) {
      const ObColumnSchemaV2 *col = *iter;
      const ObColumnSchemaV2 *new_col = nullptr;
      ObString new_col_name;
      bool is_offline = false;
      if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("col is NULL", K(ret));
      } else if (col->get_column_id() < OB_APP_MIN_COLUMN_ID) {
        // bypass hidden column
      } else if (OB_FAIL(col_name_map.get(col->get_column_name_str(), new_col_name))) {
        if (OB_ENTRY_NOT_EXIST == ret) {
          // the column is not in column name map, meaning it is dropped in this ddl
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("fail to get new column name", K(ret), K(*col));
        }
      } else if (OB_ISNULL(new_col = new_table_schema.get_column_schema(new_col_name))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("new column schema should not be null", K(ret), K(new_col_name), K(new_table_schema));
      // if this column is changed in a offline rule, meaning the data will change in this ddl, therefore
      // the column stat will be invalidated.
      } else if (OB_FAIL(data_table_schema.check_alter_column_is_offline(col,
                                                                         const_cast<ObColumnSchemaV2 *>(new_col),
                                                                         schema_guard,
                                                                         is_offline))) {
        LOG_WARN("fail to check alter table is offline", K(ret), K(*col), K(*new_col));
      } else if (!is_offline) {
        if (OB_FAIL(sync_one_column_table_level_stats_info(trans,
                                                           data_table_schema,
                                                           col->get_column_id(),
                                                           new_col->get_column_id()))) {
          LOG_WARN("fail to sync table level stats info for one column", K(ret), K(*col), K(*new_col));
        } else if (DDL_ALTER_PARTITION_BY != task_type_ &&
                   OB_FAIL(sync_one_column_partition_level_stats_info(trans,
                                                                      data_table_schema,
                                                                      new_table_schema,
                                                                      col->get_column_id(),
                                                                      new_col->get_column_id()))) {
          LOG_WARN("fail to sync partition level stats info for one column", K(ret), K(*col), K(*new_col));
        }
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_one_column_table_level_stats_info(common::ObMySQLTransaction &trans,
                                                                  const ObTableSchema &data_table_schema,
                                                                  const uint64_t old_col_id,
                                                                  const uint64_t new_col_id)
{
  int ret = OB_SUCCESS;
  ObSqlString column_sql_string;
  ObSqlString column_history_sql_string;
  ObSqlString histogram_sql_string;
  ObSqlString histogram_history_sql_string;
  int64_t affected_rows = 0;
  // for partitioned table, table-level stat is -1, for non-partitioned table, table-level stat is table id
  int64_t partition_id = -1;
  int64_t target_partition_id = -1;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (!data_table_schema.is_partitioned_table()) {
    partition_id = object_id_;
    target_partition_id = target_object_id_;
  }
  if (OB_FAIL(column_sql_string.assign_fmt("UPDATE %s SET table_id = %ld, partition_id = %ld, column_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld and column_id = %ld",
      OB_ALL_COLUMN_STAT_TNAME, target_object_id_, target_partition_id, new_col_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id,
      old_col_id))) {
    LOG_WARN("fail to assign sql string", K(ret));
  } else if (OB_FAIL(column_history_sql_string.assign_fmt(
      "UPDATE %s SET table_id = %ld, partition_id = %ld, column_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld and column_id = %ld",
      OB_ALL_COLUMN_STAT_HISTORY_TNAME, target_object_id_, target_partition_id, new_col_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id, old_col_id))) {
    LOG_WARN("fail to assign history sql string", K(ret));
  } else if (OB_FAIL(histogram_sql_string.assign_fmt("UPDATE %s SET table_id = %ld, partition_id = %ld, column_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld and column_id = %ld",
      OB_ALL_HISTOGRAM_STAT_TNAME, target_object_id_, target_partition_id, new_col_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id,
      old_col_id))) {
    LOG_WARN("fail to assign sql string", K(ret));
  } else if (OB_FAIL(histogram_history_sql_string.assign_fmt(
      "UPDATE %s SET table_id = %ld, partition_id = %ld, column_id = %ld"
      " WHERE tenant_id = %ld and table_id = %ld and partition_id = %ld and column_id = %ld",
      OB_ALL_HISTOGRAM_STAT_HISTORY_TNAME, target_object_id_, target_partition_id, new_col_id,
      ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_), object_id_, partition_id, old_col_id))) {
    LOG_WARN("fail to assign history sql string", K(ret));
  } else if (OB_FAIL(trans.write(tenant_id_, column_sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_column_stat", K(ret), K(column_sql_string));
  } else if (OB_FAIL(trans.write(tenant_id_, column_history_sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_column_stat_history", K(ret), K(column_history_sql_string));
  } else if (OB_FAIL(trans.write(tenant_id_, histogram_sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_histogram_stat_history", K(ret), K(histogram_sql_string));
  } else if (OB_FAIL(trans.write(tenant_id_, histogram_history_sql_string.ptr(), affected_rows))) {
    LOG_WARN("fail to update __all_histogram_stat_history", K(ret), K(histogram_history_sql_string));
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_one_column_partition_level_stats_info(common::ObMySQLTransaction &trans,
                                                                      const ObTableSchema &data_table_schema,
                                                                      const ObTableSchema &new_table_schema,
                                                                      const uint64_t old_col_id,
                                                                      const uint64_t new_col_id)
{
  int ret = OB_SUCCESS;
  ObArray<ObObjectID> src_partition_ids;
  ObArray<ObObjectID> dest_partition_ids;
  ObArray<ObTabletID> src_tablet_ids;
  ObArray<ObTabletID> dest_tablet_ids;
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (!data_table_schema.is_partitioned_table()) {
    // if not partition table, no need to sync partition level stats
  } else if (OB_FAIL(data_table_schema.get_all_tablet_and_object_ids(src_tablet_ids, src_partition_ids))) {
    LOG_WARN("fail to get all tablet and object ids", K(ret));
  } else if (OB_FAIL(new_table_schema.get_all_tablet_and_object_ids(dest_tablet_ids, dest_partition_ids))) {
    LOG_WARN("fail to get all tablet and object ids", K(ret));
  } else {
    const int64_t BATCH_SIZE = 256;
    int64_t batch_start = 0;
    while (OB_SUCC(ret) && batch_start < src_partition_ids.count()) {
      ObSqlString column_sql_string;
      ObSqlString column_history_sql_string;
      ObSqlString histogram_sql_string;
      ObSqlString histogram_history_sql_string;
      int64_t affected_rows = 0;
      const int64_t batch_end = std::min(src_partition_ids.count(), batch_start + BATCH_SIZE) - 1;
      if (OB_FAIL(generate_sync_column_partition_level_stats_sql(OB_ALL_COLUMN_STAT_TNAME,
                                                                 src_partition_ids,
                                                                 dest_partition_ids,
                                                                 old_col_id,
                                                                 new_col_id,
                                                                 batch_start,
                                                                 batch_end,
                                                                 column_sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(generate_sync_column_partition_level_stats_sql(OB_ALL_COLUMN_STAT_HISTORY_TNAME,
                                                                 src_partition_ids,
                                                                 dest_partition_ids,
                                                                 old_col_id,
                                                                 new_col_id,
                                                                 batch_start,
                                                                 batch_end,
                                                                 column_history_sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(generate_sync_column_partition_level_stats_sql(OB_ALL_HISTOGRAM_STAT_TNAME,
                                                                 src_partition_ids,
                                                                 dest_partition_ids,
                                                                 old_col_id,
                                                                 new_col_id,
                                                                 batch_start,
                                                                 batch_end,
                                                                 histogram_sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(generate_sync_column_partition_level_stats_sql(OB_ALL_HISTOGRAM_STAT_HISTORY_TNAME,
                                                                 src_partition_ids,
                                                                 dest_partition_ids,
                                                                 old_col_id,
                                                                 new_col_id,
                                                                 batch_start,
                                                                 batch_end,
                                                                 histogram_history_sql_string))) {
        LOG_WARN("fail to generate sql", K(ret));
      } else if (OB_FAIL(trans.write(tenant_id_, column_sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_column_stat", K(ret), K(column_sql_string));
      } else if (OB_FAIL(trans.write(tenant_id_, column_history_sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_column_stat_history", K(ret), K(column_history_sql_string));
      } else if (OB_FAIL(trans.write(tenant_id_, histogram_sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_histogram_stat_history", K(ret), K(histogram_sql_string));
      } else if (OB_FAIL(trans.write(tenant_id_, histogram_history_sql_string.ptr(), affected_rows))) {
        LOG_WARN("fail to update __all_histogram_stat_history", K(ret), K(histogram_history_sql_string));
      } else {
        batch_start += BATCH_SIZE;
      }
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::generate_sync_partition_level_stats_sql(const char *table_name,
                                                                   const ObIArray<ObObjectID> &src_partition_ids,
                                                                   const ObIArray<ObObjectID> &dest_partition_ids,
                                                                   const int64_t batch_start,
                                                                   const int64_t batch_end,
                                                                   ObSqlString &sql_string)
{
  int ret = OB_SUCCESS;
  sql_string.reset();
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (OB_UNLIKELY(src_partition_ids.count() != dest_partition_ids.count() || batch_end < batch_start
      || batch_end >= dest_partition_ids.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(src_partition_ids), K(dest_partition_ids), K(batch_start), K(batch_end));
  } else if (OB_FAIL(sql_string.append_fmt("UPDATE %s SET table_id=%ld, partition_id=(case partition_id",
        table_name, target_object_id_))) {
    LOG_WARN("fail to append sql string", K(ret), K(table_name), K(target_object_id_));
  } else {
    for (int64_t i = batch_start; OB_SUCC(ret) && i <= batch_end; i++) {
      const uint64_t src_partition_id = src_partition_ids.at(i);
      const uint64_t dest_partition_id = dest_partition_ids.at(i);
      if (OB_FAIL(sql_string.append_fmt(" when %ld then %ld", src_partition_id, dest_partition_id))) {
        LOG_WARN("fail to append sql string", K(ret), K(src_partition_id), K(dest_partition_id));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql_string.append_fmt(" else partition_id end) where tenant_id=%ld and table_id=%ld",
          ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_),
          batch_start == 0 ? object_id_ : target_object_id_))) {
      LOG_WARN("fail to append sql string", K(ret), K(object_id_), K(tenant_id_), K(exec_tenant_id));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::generate_sync_column_partition_level_stats_sql(const char *table_name,
                                                                          const ObIArray<ObObjectID> &src_partition_ids,
                                                                          const ObIArray<ObObjectID> &dest_partition_ids,
                                                                          const uint64_t old_col_id,
                                                                          const uint64_t new_col_id,
                                                                          const int64_t batch_start,
                                                                          const int64_t batch_end,
                                                                          ObSqlString &sql_string)
{
  int ret = OB_SUCCESS;
  sql_string.reset();
  const uint64_t exec_tenant_id = ObSchemaUtils::get_exec_tenant_id(tenant_id_);
  if (OB_UNLIKELY(src_partition_ids.count() != dest_partition_ids.count() || batch_end < batch_start
      || batch_end >= dest_partition_ids.count())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(src_partition_ids), K(dest_partition_ids), K(batch_start), K(batch_end));
  } else if (OB_FAIL(sql_string.append_fmt("UPDATE %s SET table_id=%ld, column_id=%ld, partition_id=(case partition_id",
        table_name, target_object_id_, new_col_id))) {
    LOG_WARN("fail to append sql string", K(ret), K(table_name), K(target_object_id_), K(new_col_id));
  } else {
    for (int64_t i = batch_start; OB_SUCC(ret) && i <= batch_end; i++) {
      const uint64_t src_partition_id = src_partition_ids.at(i);
      const uint64_t dest_partition_id = dest_partition_ids.at(i);
      if (OB_FAIL(sql_string.append_fmt(" when %ld then %ld", src_partition_id, dest_partition_id))) {
        LOG_WARN("fail to append sql string", K(ret), K(src_partition_id), K(dest_partition_id));
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql_string.append_fmt(" else partition_id end) where tenant_id=%ld and table_id=%ld and column_id=%ld",
          ObSchemaUtils::get_extract_tenant_id(exec_tenant_id, tenant_id_),
          batch_start == 0 ? object_id_ : target_object_id_, old_col_id))) {
      LOG_WARN("fail to append sql string", K(ret), K(object_id_), K(tenant_id_), K(exec_tenant_id), K(old_col_id));
    }
  }
  return ret;
}

int ObDDLRedefinitionTask::sync_tablet_autoinc_seq()
{
  int ret = OB_SUCCESS;
  if (!sync_tablet_autoinc_seq_ctx_.is_inited()
      && OB_FAIL(sync_tablet_autoinc_seq_ctx_.init(tenant_id_, object_id_, target_object_id_))) {
    LOG_WARN("failed to init sync tablet autoinc seq ctx", K(ret));
  } else if (OB_FAIL(sync_tablet_autoinc_seq_ctx_.sync())) {
    LOG_WARN("failed to sync tablet autoinc seq", K(ret));
  }
  return ret;
}

int ObDDLRedefinitionTask::check_need_rebuild_constraint(const ObTableSchema &table_schema,
                                                         ObIArray<uint64_t> &constraint_ids,
                                                         bool &need_rebuild_constraint)
{
  int ret = OB_SUCCESS;
  int64_t new_constraint_cnt = 0;
  const AlterTableSchema &alter_table_schema = alter_table_arg_.alter_table_schema_;
  if (obrpc::ObAlterTableArg::ADD_CONSTRAINT == alter_table_arg_.alter_constraint_type_
             || obrpc::ObAlterTableArg::ALTER_CONSTRAINT_STATE == alter_table_arg_.alter_constraint_type_) {
    for (ObTableSchema::const_constraint_iterator iter = alter_table_schema.constraint_begin();
        OB_SUCC(ret) && iter != alter_table_schema.constraint_end(); ++iter) {
      if (OB_ISNULL(*iter)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("iter is NULL", K(ret));
      } else if ((*iter)->get_constraint_type() != CONSTRAINT_TYPE_PRIMARY_KEY) {
        new_constraint_cnt++;
      }
    }
  }
  if (OB_SUCC(ret)) {
    for (ObTableSchema::const_constraint_iterator iter = table_schema.constraint_begin();
        OB_SUCC(ret) && iter != table_schema.constraint_end(); ++iter) {
      if (OB_ISNULL(*iter)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("iter is NULL", K(ret));
      } else if ((*iter)->get_constraint_type() != CONSTRAINT_TYPE_PRIMARY_KEY) {
        const uint64_t constraint_id = (*iter)->get_constraint_id();
        if (OB_FAIL(constraint_ids.push_back(constraint_id))) {
          LOG_WARN("push back constraint id failed", K(ret));
        } else {
          LOG_INFO("constraint already built", K(**iter), K(constraint_ids));
        }
      }
    }
  }
  need_rebuild_constraint = constraint_ids.count() == new_constraint_cnt;
  return ret;
}        

int ObDDLRedefinitionTask::check_need_check_table_empty(bool &need_check_table_empty)
{
  int ret = OB_SUCCESS;
  need_check_table_empty = false;
  const AlterTableSchema &alter_table_schema = alter_table_arg_.alter_table_schema_;
  ObTableSchema::const_column_iterator it_begin = alter_table_schema.column_begin();
  ObTableSchema::const_column_iterator it_end = alter_table_schema.column_end();
  AlterColumnSchema *alter_column_schema = NULL;
  for(; OB_SUCC(ret) && !need_check_table_empty && it_begin != it_end; it_begin++) {
    if (OB_ISNULL(*it_begin)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("*it_begin is NULL", K(ret));
    } else {
      alter_column_schema = static_cast<AlterColumnSchema *>(*it_begin);
      if (OB_DDL_ADD_COLUMN == alter_column_schema->alter_type_
          && alter_column_schema->has_not_null_constraint()
          && alter_column_schema->get_orig_default_value().is_null()
          && !alter_column_schema->is_identity_column()) {
        need_check_table_empty = true;
      }
    }
  }
  return ret;
}

ObSyncTabletAutoincSeqCtx::ObSyncTabletAutoincSeqCtx()
  : is_inited_(false), is_synced_(false), tenant_id_(OB_INVALID_ID), orig_src_tablet_ids_(),
    src_tablet_ids_(), dest_tablet_ids_(), autoinc_params_()
{}

int ObSyncTabletAutoincSeqCtx::init(uint64_t tenant_id, int64_t src_table_id, int64_t dest_table_id)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(tenant_id == OB_INVALID_ID || src_table_id == OB_INVALID_ID || dest_table_id == OB_INVALID_ID)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(tenant_id), K(src_table_id), K(dest_table_id));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id, src_table_id, orig_src_tablet_ids_))) {
    LOG_WARN("failed to get data table snapshot", K(ret));
  } else if (OB_FAIL(src_tablet_ids_.assign(orig_src_tablet_ids_))) {
    LOG_WARN("failed to assign src_tablet_ids", K(ret));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(tenant_id, dest_table_id, dest_tablet_ids_))) {
    LOG_WARN("failed to get dest table snapshot", K(ret));
  } else {
    tenant_id_ = tenant_id;
    is_synced_ = false;
    is_inited_ = true;
  }
  return ret;
}

int ObSyncTabletAutoincSeqCtx::sync()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("ObSyncTabletAutoincSeqCtx has not been inited", K(ret));
  } else if (!is_synced_) {
    obrpc::ObSrvRpcProxy *srv_rpc_proxy = nullptr;
    if (OB_ISNULL(srv_rpc_proxy = GCTX.srv_rpc_proxy_)) {
      ret = OB_ERR_SYS;
      LOG_WARN("rpc proxy or location_cache is null", K(ret), KP(srv_rpc_proxy));
    } else {
      while (OB_SUCC(ret) && src_tablet_ids_.count() > 0) {
        ObBatchGetTabletAutoincSeqProxy proxy(*srv_rpc_proxy, &obrpc::ObSrvRpcProxy::batch_get_tablet_autoinc_seq);
        obrpc::ObBatchGetTabletAutoincSeqArg arg;
        if (OB_FAIL(call_and_process_all_tablet_autoinc_seqs(proxy, arg, true/*is_get*/))) {
          LOG_WARN("failed to call and process", K(ret));
        }
      }
      while (OB_SUCC(ret) && autoinc_params_.count() > 0) {
        ObBatchSetTabletAutoincSeqProxy proxy(*srv_rpc_proxy, &obrpc::ObSrvRpcProxy::batch_set_tablet_autoinc_seq);
        obrpc::ObBatchSetTabletAutoincSeqArg arg;
        if (OB_FAIL(call_and_process_all_tablet_autoinc_seqs(proxy, arg, false/*is_get*/))) {
          LOG_WARN("failed to call and process", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        is_synced_ = true;
      }
    }
  }
  return ret;
}

int ObSyncTabletAutoincSeqCtx::build_ls_to_tablet_map(
    share::ObLocationService *location_service,
    const uint64_t tenant_id,
    const ObIArray<ObMigrateTabletAutoincSeqParam> &autoinc_params,
    const int64_t timeout,
    const bool force_renew,
    const bool by_src_tablet,
    ObHashMap<ObLSID, ObSEArray<ObMigrateTabletAutoincSeqParam, 1>> &map)
{
  int ret = OB_SUCCESS;
  map.reuse();
  bool is_cache_hit = false;
  const int64_t expire_renew_time = force_renew ? INT64_MAX : 0;
  ObTimeoutCtx timeout_ctx;
  if (nullptr == location_service || OB_INVALID_ID == tenant_id || autoinc_params.count() <= 0 || timeout <= 0) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid arguments", K(ret), KP(location_service), K(tenant_id), K(autoinc_params), K(timeout));
  } else if (OB_FAIL(timeout_ctx.set_trx_timeout_us(timeout))) {
    LOG_WARN("set trx timeout failed", K(ret));
  } else if (OB_FAIL(timeout_ctx.set_timeout(timeout))) {
    LOG_WARN("set timeout failed", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < autoinc_params.count(); i++) {
      ObLSID ls_id;
      const ObMigrateTabletAutoincSeqParam &autoinc_param = autoinc_params.at(i);
      const ObTabletID &tablet_id = by_src_tablet ? autoinc_param.src_tablet_id_ : autoinc_param.dest_tablet_id_;
      ObSEArray<ObMigrateTabletAutoincSeqParam, 1> tmp_list;
      if (OB_FAIL(location_service->get(tenant_id, tablet_id, expire_renew_time, is_cache_hit, ls_id))) {
        LOG_WARN("fail to get log stream id", K(ret), K(tablet_id));
      } else if (OB_FAIL(map.get_refactored(ls_id, tmp_list))) {
        if (OB_HASH_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
        } else {
          LOG_WARN("failed to get from map", K(ret), K(ls_id));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(tmp_list.push_back(autoinc_param))) {
        LOG_WARN("failed to push tablet id", K(ret), K(tablet_id));
      } else if (OB_FAIL(map.set_refactored(ls_id, tmp_list, 1/*overwrite*/))) {
        LOG_WARN("failed to set map", K(ret), K(ls_id), K(tmp_list));
      }
    }
  }
  return ret;
}

template<typename P, typename A>
int ObSyncTabletAutoincSeqCtx::call_and_process_all_tablet_autoinc_seqs(P &proxy, A &arg, const bool is_get)
{
  int ret = OB_SUCCESS;
  const int64_t rpc_timeout = max(GCONF.rpc_timeout, 1000L * 1000L * 9L);
  const bool force_renew = false;
  share::ObLocationService *location_service = nullptr;
  ObHashMap<ObLSID, ObSEArray<ObMigrateTabletAutoincSeqParam, 1>> ls_to_tablet_map;
  if (OB_ISNULL(location_service = GCTX.location_service_)) {
    ret = OB_ERR_SYS;
    LOG_WARN("location_cache is null", K(ret));
  } else if (OB_FAIL(ls_to_tablet_map.create(MAP_BUCKET_NUM, "DDLRedefTmp"))) {
    LOG_WARN("failed to create map", K(ret));
  } else {
    if (is_get) {
      ObSEArray<ObMigrateTabletAutoincSeqParam, 1> tmp_autoinc_params;
      if (OB_UNLIKELY(src_tablet_ids_.count() != dest_tablet_ids_.count())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("invalid tablet ids count", K(ret), K(src_tablet_ids_), K(dest_tablet_ids_));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < src_tablet_ids_.count(); i++) {
        ObMigrateTabletAutoincSeqParam autoinc_param;
        autoinc_param.src_tablet_id_ = src_tablet_ids_.at(i);
        autoinc_param.dest_tablet_id_ = dest_tablet_ids_.at(i);
        if (OB_FAIL(tmp_autoinc_params.push_back(autoinc_param))) {
          LOG_WARN("failed to push back", K(ret), K(autoinc_param));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(build_ls_to_tablet_map(location_service,
                                                         tenant_id_,
                                                         tmp_autoinc_params,
                                                         rpc_timeout,
                                                         force_renew,
                                                         true/*by src tablet*/,
                                                         ls_to_tablet_map))) {
        LOG_WARN("failed to build ls to tabmap", K(ret));
      }
    } else {
      if (OB_FAIL(build_ls_to_tablet_map(location_service,
                                         tenant_id_,
                                         autoinc_params_,
                                         rpc_timeout,
                                         force_renew,
                                         false/*by src tablet*/,
                                         ls_to_tablet_map))) {
        LOG_WARN("failed to build ls to tabmap", K(ret));
      }
    }
  }

  // prepeare rpc arg
  if (OB_SUCC(ret)) {
    ObHashMap<ObLSID, ObSEArray<ObMigrateTabletAutoincSeqParam, 1>>::hashtable::const_iterator map_iter = ls_to_tablet_map.begin();
    for (; OB_SUCC(ret) && map_iter != ls_to_tablet_map.end(); ++map_iter) {
      const ObLSID &ls_id = map_iter->first;
      ObAddr leader_addr;
      if (OB_FAIL(location_service->get_leader(GCONF.cluster_id,
                                               tenant_id_,
                                               ls_id,
                                               force_renew,
                                               leader_addr))) {
        LOG_WARN("failed to get leader", K(ret));
      } else if (OB_FAIL(arg.init(tenant_id_, ls_id, map_iter->second))) {
        LOG_WARN("failed to init arg", K(ret));
      } else if (OB_FAIL(proxy.call(leader_addr, rpc_timeout, tenant_id_, arg))) {
        LOG_WARN("send rpc failed", K(ret), K(arg), K(leader_addr));
      }
    }

    // wait rpc and process result
    int tmp_ret = OB_SUCCESS;
    common::ObArray<int> tmp_ret_array;
    if (OB_SUCCESS != (tmp_ret = proxy.wait_all(tmp_ret_array))) {
      LOG_WARN("rpc proxy wait failed", K(tmp_ret));
      ret = OB_SUCCESS == ret ? tmp_ret : ret;
    } else if (OB_SUCC(ret)) {
      const auto &result_array = proxy.get_results();
      if (tmp_ret_array.count() != ls_to_tablet_map.size() || result_array.count() != ls_to_tablet_map.size()) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("result count not match", K(ret), K(ls_to_tablet_map.size()), K(tmp_ret_array.count()), K(result_array.count()));
      } else {
        ObHashMap<ObLSID, ObSEArray<ObMigrateTabletAutoincSeqParam, 1>>::hashtable::iterator map_iter = ls_to_tablet_map.begin();
        int64_t new_params_cnt = 0;

        // check and reserve first
        for (int64_t i = 0; OB_SUCC(ret) && i < result_array.count(); ++i, ++map_iter) {
          const int rpc_ret_code = tmp_ret_array.at(i);
          const auto *cur_result = result_array.at(i);
          if (OB_ISNULL(cur_result) || OB_UNLIKELY(map_iter == ls_to_tablet_map.end())) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("result it null", K(ret), K(i), KP(cur_result));
          } else if (OB_SUCCESS == rpc_ret_code) {
            if (OB_UNLIKELY(map_iter->second.count() != cur_result->autoinc_params_.count())) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("result params count must be equal to request params count when rpc succ",
                K(ret), K(map_iter->second.count()), K(cur_result->autoinc_params_.count()));
            }
            for (int64_t j = 0; OB_SUCC(ret) && j < cur_result->autoinc_params_.count(); j++) {
              const ObMigrateTabletAutoincSeqParam &autoinc_param = cur_result->autoinc_params_.at(j);
              if (OB_SUCCESS == autoinc_param.ret_code_) {
                if (is_get) {
                  new_params_cnt++;
                }
              } 
            }
          }
        }
        if (OB_FAIL(ret)) {
        } else if (is_get && OB_FAIL(autoinc_params_.reserve(new_params_cnt))) {
          LOG_WARN("failed to reserve new param cnt", K(ret));
        }

        // the following won't fail
        if (OB_SUCC(ret)) {
          if (is_get) {
            src_tablet_ids_.reuse();
            dest_tablet_ids_.reuse();
          } else {
            autoinc_params_.reuse();
          }
          tmp_ret = OB_SUCCESS; // last non-retry error or first retry error or success
          map_iter = ls_to_tablet_map.begin();
          for (int64_t i = 0; OB_SUCC(ret) && i < result_array.count(); ++i, ++map_iter) {
            const int rpc_ret_code = tmp_ret_array.at(i);
            const auto *cur_result = result_array.at(i);
            const ObIArray<ObMigrateTabletAutoincSeqParam> *result_params = nullptr;
            if (OB_SUCCESS == rpc_ret_code) {
              result_params = &cur_result->autoinc_params_;
            } else {
              for (int64_t j = 0; j < map_iter->second.count(); j++) {
                ObMigrateTabletAutoincSeqParam &autoinc_param = map_iter->second.at(j);
                autoinc_param.ret_code_ = rpc_ret_code;
              }
              result_params = &map_iter->second;
            }
            for (int64_t j = 0; OB_SUCC(ret) && j < result_params->count(); j++) {
              const ObMigrateTabletAutoincSeqParam &autoinc_param = result_params->at(j);
              if (OB_SUCCESS == autoinc_param.ret_code_) {
                if (is_get) {
                  if (OB_FAIL(autoinc_params_.push_back(autoinc_param))) {
                    LOG_WARN("failed to push autoinc param", K(ret), K(autoinc_param));
                  }
                } else {
                  // do nothing
                }
              } else {
                // reclaim on failure
                if (is_get) {
                  if (OB_FAIL(src_tablet_ids_.push_back(autoinc_param.src_tablet_id_))) {
                    LOG_WARN("failed to push src tablet id", K(ret), K(autoinc_param));
                  } else if (OB_FAIL(dest_tablet_ids_.push_back(autoinc_param.dest_tablet_id_))) {
                    LOG_WARN("failed to push dest tablet id", K(ret), K(autoinc_param));
                  }
                } else {
                  if (OB_FAIL(autoinc_params_.push_back(autoinc_param))) {
                    LOG_WARN("failed to push autoinc param", K(ret));
                  }
                }
                if (is_error_need_retry(autoinc_param.ret_code_)) {
                  if (tmp_ret == OB_SUCCESS) {
                    tmp_ret = autoinc_param.ret_code_;
                  }
                } else {
                  tmp_ret = autoinc_param.ret_code_;
                }
              }
            }
          }
          if (OB_SUCC(ret)) {
            ret = tmp_ret;
          }
        }
      }
    }

    if (ls_to_tablet_map.created()) {
      ls_to_tablet_map.destroy();
    }
  }
  if (OB_SUCC(ret) && is_get) {
    if (OB_UNLIKELY(orig_src_tablet_ids_.count() != autoinc_params_.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid count", K(orig_src_tablet_ids_), K(autoinc_params_));
    }
  }
  return ret;
}
