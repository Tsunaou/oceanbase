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

#define USING_LOG_PREFIX RPC_OBMYSQL
#include "observer/mysql/obsm_conn_callback.h"
#include "rpc/obmysql/ob_sql_sock_session.h"
#include "rpc/obmysql/obsm_struct.h"
#include "rpc/obmysql/ob_mysql_packet.h"
#include "rpc/obmysql/packet/ompk_handshake.h"
#include "lib/random/ob_mysql_random.h"
#include "observer/ob_server_struct.h"
#include "sql/session/ob_sql_session_mgr.h"
#include "observer/omt/ob_tenant.h"
#include "observer/ob_srv_task.h"

namespace oceanbase
{
using namespace common;
using namespace observer;
namespace obmysql
{

static int create_scramble_string(char *scramble_buf, const int64_t buf_len, common::ObMysqlRandom &thread_rand)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!thread_rand.is_inited())) {
    if (OB_UNLIKELY(!GCTX.scramble_rand_->is_inited())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_ERROR("global_rand has not inited, it should not happened", K(ret));
    } else {
      // Concurrent access by multiple threads maybe happened here, but we do not care
      const uint64_t tmp_seed = GCTX.scramble_rand_->get_uint64();
      thread_rand.init(tmp_seed + reinterpret_cast<uint64_t>(&thread_rand),
                       tmp_seed + static_cast<uint64_t>(ObTimeUtility::current_time()));
      LOG_INFO("init thread_rand succ", K(ret));
    }
  }

  if (FAILEDx(thread_rand.create_random_string(scramble_buf, buf_len))) {
    LOG_ERROR("fail to create_random_string", K(scramble_buf), K(buf_len), K(ret));
  }
  return ret;
}

static int send_handshake(ObSqlSockSession& sess, const OMPKHandshake &hsp)
{
  int ret = OB_SUCCESS;
  static const int64_t MAX_HSPKT_SIZE = 128;
  char buf[MAX_HSPKT_SIZE];
  const int64_t len = MAX_HSPKT_SIZE;
  int64_t pos = 0;
  int64_t pkt_count = 0;

  if (OB_FAIL(hsp.encode(buf, len, pos, pkt_count))) {
    LOG_WARN("encode handshake packet fail", K(ret));
  } else if (OB_UNLIKELY(pkt_count <= 0)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid pkt count", K(pkt_count), K(ret));
  } else if (OB_FAIL(sess.write_data(buf, pos))) {
    LOG_WARN("write handshake packet data fail", K(ret));
  }

  return ret;
}

static int sm_conn_init(ObSMConnection& conn)
{
  int ret = OB_SUCCESS;
  int crt_id_ret = OB_SUCCESS;
  uint32_t sessid = 0;
  crt_id_ret =  GCTX.session_mgr_->create_sessid(sessid);
  if (OB_UNLIKELY(OB_SUCCESS != crt_id_ret && OB_ERR_CON_COUNT_ERROR != crt_id_ret)) {
    ret = crt_id_ret;
    LOG_WARN("fail to create sessid", K(crt_id_ret), K(sessid));
  } else {
    conn.sessid_ = sessid;
    conn.ret_ = crt_id_ret;
  }
  return ret;
}

static int sm_conn_build_handshake(ObSMConnection& conn, obmysql::OMPKHandshake& hsp)
{
  int ret = OB_SUCCESS;
  RLOCAL(common::ObMysqlRandom, thread_scramble_rand);
  hsp.set_thread_id(conn.sessid_);
  const bool support_ssl = GCONF.ssl_client_authentication;
  hsp.set_ssl_cap(support_ssl);
  const int64_t BUF_LEN = sizeof(conn.scramble_buf_);
  if (OB_FAIL(create_scramble_string(conn.scramble_buf_, BUF_LEN, thread_scramble_rand))) {
    LOG_WARN("create scramble string failed", K(ret));
  } else if (OB_FAIL(hsp.set_scramble(conn.scramble_buf_, BUF_LEN))) {
    LOG_WARN("set scramble failed", K(ret));
  } else {
    LOG_INFO("new mysql sessid created", K(conn.sessid_), K(support_ssl));
  }
  return ret;
}

int ObSMConnectionCallback::init(ObSqlSockSession& sess, ObSMConnection& conn)
{
  int ret = OB_SUCCESS;
  obmysql::OMPKHandshake hsp;
  if (OB_FAIL(sm_conn_init(conn))) {
    LOG_WARN("init conn fail", K(ret));
  } else if (OB_FAIL(sm_conn_build_handshake(conn, hsp))) {
    LOG_WARN("conn send handshake fail", K(ret));
  } else if (OB_FAIL(send_handshake(sess, hsp))) {
    LOG_WARN("send handshake fail", K(ret), K(sess.client_addr_));
  } else {
    LOG_INFO("sm conn init succ", K(conn.sessid_), K(sess.client_addr_));
  }
  if (OB_SUCCESS != ret && OB_SUCCESS == conn.ret_) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = GCTX.session_mgr_->mark_sessid_unused(conn.sessid_))) {
      LOG_ERROR("fail to mark sessid unused", K(tmp_ret), K(conn.sessid_));
    }
  }

  //如果当前function发生错误，应该在当前function中进行mark_sessid_unused
  if (OB_SUCCESS == ret && OB_SUCCESS == conn.ret_) {
    conn.is_need_clear_sessid_ = true;
  }
  return ret;
}

static void sm_conn_unlock_tenant(ObSMConnection& conn)
{
  //unlock tenant
  if (NULL != conn.tenant_ && conn.is_tenant_locked_) {
    conn.tenant_->unlock(*conn.handle_);
    conn.is_tenant_locked_ = false;
    conn.tenant_ = NULL;
    LOG_INFO("unlock session of tenant",K(conn.sessid_),
             "proxy_sessid", conn.proxy_sessid_, K(conn.tenant_id_));
  }
}

static void sm_conn_log_close(ObSMConnection& conn, int ret)
{
  LOG_INFO("connection close",
           "sessid", conn.sessid_,
           "proxy_sessid", conn.proxy_sessid_,
           "tenant_id", conn.tenant_id_,
           "server_id", GCTX.server_id_,
           "from_proxy", conn.is_proxy_,
           "from_java_client", conn.is_java_client_,
           "c/s protocol", get_cs_protocol_type_name(conn.get_cs_protocol_type()),
           "is_need_clear_sessid_", conn.is_need_clear_sessid_,
           K(ret));
}

void ObSMConnectionCallback::destroy(ObSMConnection& conn)
{
  int ret = OB_SUCCESS;
  bool is_need_clear = false;
  if (conn.is_sess_alloc_ && !conn.is_sess_free_) {
    sql::ObFreeSessionCtx ctx;
    ctx.tenant_id_ = conn.tenant_id_;
    ctx.sessid_ = conn.sessid_;
    ctx.proxy_sessid_ = conn.proxy_sessid_;
    ctx.has_inc_active_num_ = conn.has_inc_active_num_;

    //free session in task
    ObSrvTask *task = OB_NEW(ObDisconnectTask,
                              ObModIds::OB_RPC,
                              ctx);
    if (OB_UNLIKELY(NULL == task)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_UNLIKELY(NULL == conn.tenant_)) {
      ret = OB_TENANT_NOT_EXIST;
    } else if (OB_FAIL(conn.tenant_->recv_request(*task))) {
      LOG_WARN("push disconnect task fail", K(conn.sessid_),
                "proxy_sessid", conn.proxy_sessid_, K(ret));
      ob_delete(task);
    }
    // free session locally
    if (OB_FAIL(ret)) {
      int tmp_ret = GCTX.session_mgr_->free_session(ctx);
      if (OB_UNLIKELY(OB_SUCCESS != tmp_ret)) {
        LOG_WARN("free session fail", K(ctx), K(tmp_ret));
        ret = tmp_ret;
      } else {
        LOG_INFO("free session successfully", K(conn.sessid_),
                  "proxy_sessid", conn.proxy_sessid_, K(ctx));
      }
    }
  }

  if (OB_UNLIKELY(OB_FAIL(sql::ObSQLSessionMgr::is_need_clear_sessid(&conn, is_need_clear)))) {
    LOG_ERROR("fail to jugde need clear", K(ret));
  } else if (is_need_clear) {
    GCTX.session_mgr_->mark_sessid_unused(conn.sessid_);
  }
  sm_conn_unlock_tenant(conn);
  sm_conn_log_close(conn, ret);
  conn.~ObSMConnection();
}

int ObSMConnectionCallback::on_disconnect(observer::ObSMConnection& conn)
{
  int ret = OB_SUCCESS;

  if (conn.is_sess_alloc_
      && !conn.is_sess_free_
      && ObSMConnection::INITIAL_SESSID != conn.sessid_) {
    sql::ObSQLSessionInfo *sess_info = NULL;
    sql::ObSessionGetterGuard guard(*(GCTX.session_mgr_), conn.sessid_);
    if (OB_FAIL(guard.get_session(sess_info))) {
      LOG_WARN("fail to get session", K(conn.sessid_),
                "proxy_sessid", conn.proxy_sessid_);
    } else if (OB_ISNULL(sess_info)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("session info is NULL", K(conn.sessid_),
                "proxy_sessid", conn.proxy_sessid_);
    } else {
      sess_info->set_session_state(sql::SESSION_KILLED);
      sess_info->set_shadow(true);
    }
  }
  LOG_INFO("kill and revert session", K(conn.sessid_),
      "proxy_sessid", conn.proxy_sessid_, "server_id", GCTX.server_id_, K(ret));
  return ret;
}

ObSMConnectionCallback global_sm_conn_callback;
}; // end namespace mysql
}; // end namespace oceanbase
