--package_name:dbms_workload_repository
--author:xiaochu.yh

CREATE OR REPLACE PACKAGE BODY dbms_workload_repository AS

TYPE COLUMN_CONTENT_ARRAY IS VARRAY(20) OF VARCHAR2(4096);
TYPE COLUMN_WIDTH_ARRAY IS VARRAY(20) OF INTEGER;

DIG_3_FM        VARCHAR2(20) := 'FM999999990.000';
DIG_2_FM        VARCHAR2(20) := 'FM999999990.00';

-- helper functions
PROCEDURE APPEND_ROW(ROW IN VARCHAR2)
IS
BEGIN
   RPT_ROWS(RPT_ROWS.COUNT) := ROW;
END APPEND_ROW;

PROCEDURE REPORT_CLEANUP
IS
BEGIN
  IF (RPT_ROWS.COUNT > 0) THEN
     RPT_ROWS.DELETE;
  END IF;
END REPORT_CLEANUP;

FUNCTION FORMAT_ROW(column_content IN COLUMN_CONTENT_ARRAY,
                    column_width IN COLUMN_WIDTH_ARRAY,
                    pad IN VARCHAR2,
                    sep IN VARCHAR2)
RETURN VARCHAR2
IS
  RES VARCHAR2(4000 CHAR);
BEGIN
  RES := '';
  FOR i IN 1 .. column_content.count LOOP
    RES := RES || LPAD(column_content(i), column_width(i), pad) || sep;
  END LOOP;
  RETURN RES;
END FORMAT_ROW;


-- main function
FUNCTION ASH_REPORT_TEXT(l_btime         IN DATE,
                         l_etime         IN DATE,
                         l_sql_id        IN VARCHAR2  DEFAULT NULL,
                         l_wait_class    IN VARCHAR2  DEFAULT NULL
                        )
RETURN awrrpt_text_type_table
IS
  DYN_SQL          VARCHAR2(15000);
  NULL_NUM         NUMBER := NULL;
  NULL_CHAR        VARCHAR2(10) := NULL;

  TYPE TopEventCursor IS REF CURSOR;
  top_event_cv        TopEventCursor;

  TYPE TopEventRecord IS RECORD (
    EVENT             SYS.V$ACTIVE_SESSION_HISTORY.EVENT%TYPE,
    WAIT_CLASS        SYS.V$ACTIVE_SESSION_HISTORY.WAIT_CLASS%TYPE,
    EVENT_CNT         NUMBER
  );
  top_event_rec       TopEventRecord;

  TYPE TopEventPvalRecord IS RECORD (
    EVENT             SYS.V$ACTIVE_SESSION_HISTORY.EVENT%TYPE,
    EVENT_CNT         NUMBER,
    SAMPLE_CNT        NUMBER,
    P1             SYS.V$ACTIVE_SESSION_HISTORY.P1%TYPE,
    P2             SYS.V$ACTIVE_SESSION_HISTORY.P2%TYPE,
    P3             SYS.V$ACTIVE_SESSION_HISTORY.P3%TYPE,
    P1TEXT         SYS.V$ACTIVE_SESSION_HISTORY.P1TEXT%TYPE,
    P2TEXT         SYS.V$ACTIVE_SESSION_HISTORY.P2TEXT%TYPE,
    P3TEXT         SYS.V$ACTIVE_SESSION_HISTORY.P3TEXT%TYPE
  );
  top_event_pval_rec  TopEventPvalRecord;

  TYPE TopAppInfoRecord IS RECORD (
    MODULE         SYS.V$ACTIVE_SESSION_HISTORY.MODULE%TYPE,
    ACTION         SYS.V$ACTIVE_SESSION_HISTORY.ACTION%TYPE,
    SAMPLE_CNT     NUMBER
  );
  top_appinfo_rec  TopAppInfoRecord;

  TYPE TopPhaseOfExecutionRecord IS RECORD (
    EXECUTION_PHASE   VARCHAR2(40),
    SAMPLE_CNT        NUMBER
  );
  top_phase_rec   TopPhaseOfExecutionRecord;

  TYPE TopSQLRecord IS RECORD (
    SQL_ID         SYS.V$ACTIVE_SESSION_HISTORY.SQL_ID%TYPE,
    PLAN_ID        NUMBER,
    EVENT_CNT      NUMBER,
    EVENT          SYS.V$ACTIVE_SESSION_HISTORY.EVENT%TYPE,
    QUERY_SQL      SYS.V$OB_PLAN_CACHE_PLAN_STAT.QUERY_SQL%TYPE
  );
  top_sql_rec   TopSQLRecord;

  TYPE CompleteSQLRecord IS RECORD (
    SQL_ID         SYS.V$ACTIVE_SESSION_HISTORY.SQL_ID%TYPE,
    QUERY_SQL      SYS.V$OB_PLAN_CACHE_PLAN_STAT.QUERY_SQL%TYPE
  );
  complete_sql_rec   CompleteSQLRecord;


  TYPE TopSessionRecord IS RECORD (
    SESSION_ID        SYS.V$ACTIVE_SESSION_HISTORY.SESSION_ID%TYPE,
    EVENT             SYS.V$ACTIVE_SESSION_HISTORY.EVENT%TYPE,
    EVENT_CNT         NUMBER,
    SAMPLE_CNT        NUMBER,
    USER_NAME         SYS.ALL_USERS.USERNAME%TYPE
  );
  top_sess_rec       TopSessionRecord;

  TYPE TopLatchRecord IS RECORD (
    EVENT             SYS.V$ACTIVE_SESSION_HISTORY.EVENT%TYPE,
    SAMPLE_CNT        NUMBER
  );
  top_latch_rec       TopLatchRecord;




  column_content COLUMN_CONTENT_ARRAY;
  column_widths COLUMN_WIDTH_ARRAY;

  ASH_END_TIME     Date;
  ASH_BEGIN_TIME   Date;
  DUR_ELAPSED      Number;
  NUM_SAMPLES      Number;
  NUM_EVENTS       Number; -- One event may cross many samples

  FILTER_EVENT_STR CONSTANT VARCHAR2(100) := 'CASE WHEN wait_class_id = 100 OR TIME_WAITED != 0 THEN 1 ELSE 0 END';
BEGIN
  REPORT_CLEANUP();

  DBMS_OUTPUT.PUT_LINE('');
  DBMS_OUTPUT.PUT_LINE('# ' || l_sql_id);
  DBMS_OUTPUT.PUT_LINE('');

  DYN_SQL := 'SELECT MIN(SAMPLE_TIME) ASH_BEGIN_TIME, MAX(SAMPLE_TIME) ASH_END_TIME, COUNT(1) NUM_SAMPLES, SUM(' || FILTER_EVENT_STR || ') NUM_EVENTS ' ||
             'FROM   (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ';
  EXECUTE IMMEDIATE DYN_SQL
  INTO  ASH_BEGIN_TIME, ASH_END_TIME, NUM_SAMPLES, NUM_EVENTS
  USING L_BTIME, L_ETIME,
        L_BTIME, L_ETIME,
        NULL_CHAR, NULL_CHAR,
        NULL_CHAR, NULL_CHAR,
        NULL_CHAR, NULL_CHAR,
        NULL_CHAR, NULL_CHAR,
        NULL_CHAR, NULL_CHAR,
        NULL_CHAR, NULL_CHAR;
  DUR_ELAPSED    := (ASH_END_TIME - ASH_BEGIN_TIME) * 24 * 60 * 60; -- in seconds
  IF DUR_ELAPSED <= 0 THEN
    DUR_ELAPSED := 1; -- avoid zero division
  END IF;
  IF NUM_SAMPLES <= 0 THEN
    NUM_SAMPLES := 1;
  END IF;
  IF NUM_EVENTS <= 0 THEN
    NUM_EVENTS := 1;
  END IF;

  APPEND_ROW('----');
  APPEND_ROW('           Sample Begin: ' || TO_CHAR(L_BTIME, 'yyyy-mm-dd HH24:MI:SS'));
  APPEND_ROW('             Sample End: ' || TO_CHAR(L_ETIME, 'yyyy-mm-dd HH24:MI:SS'));
  APPEND_ROW('             ----------');
  APPEND_ROW('    Analysis Begin Time: ' || TO_CHAR(ASH_BEGIN_TIME, 'yyyy-mm-dd HH24:MI:SS'));
  APPEND_ROW('      Analysis End Time: ' || TO_CHAR(ASH_END_TIME, 'yyyy-mm-dd HH24:MI:SS'));
  APPEND_ROW('           Elapsed Time: ' || TO_CHAR(DUR_ELAPSED) || '(secs)');
  APPEND_ROW('          Num of Sample: ' || TO_CHAR(NUM_SAMPLES));
  APPEND_ROW('          Num of Events: ' || TO_CHAR(NUM_EVENTS));
  APPEND_ROW('Average Active Sessions: ' || TO_CHAR(ROUND(NUM_SAMPLES/DUR_ELAPSED,2), DIG_3_FM));
  APPEND_ROW('----');

  APPEND_ROW(' ');
  APPEND_ROW('## Top User Events:');
  column_widths := COLUMN_WIDTH_ARRAY(40, 20, 10, 9);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Event', 'WAIT_CLASS', 'EVENT_CNT', '% Event');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT EVENT,  WAIT_CLASS, COUNT(1) EVENT_CNT FROM (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' || 'GROUP BY EVENT, WAIT_CLASS';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
    FETCH top_event_cv INTO top_event_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          top_event_rec.EVENT,
          top_event_rec.WAIT_CLASS,
          TO_CHAR(top_event_rec.EVENT_CNT),
          TO_CHAR(ROUND(100 * top_event_rec.EVENT_CNT/NUM_EVENTS,2), DIG_2_FM) || '%'
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));


  APPEND_ROW(' ');
  APPEND_ROW('## Top Events P1/P2/P3 Value:');
  column_widths := COLUMN_WIDTH_ARRAY(40, 10, 12, 50, 20, 20, 20);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Event', '% Event', '% Activity', 'Max P1/P2/P3', 'Parameter 1', 'Parameter 2', 'Parameter 3');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT * FROM (SELECT EVENT, SUM(' || FILTER_EVENT_STR || ') EVENT_CNT, COUNT(1) SAMPLE_CNT, MAX(P1) P1, MAX(P2) P2, MAX(P3) P3, MAX(P1TEXT) P1TEXT, MAX(P2TEXT) P2TEXT, MAX(P3TEXT) P3TEXT ' ||
             'FROM   (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' ||
             'GROUP BY EVENT, WAIT_CLASS ORDER BY 2 DESC) WHERE ROWNUM < 10';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
      FETCH top_event_cv INTO top_event_pval_rec;
      EXIT WHEN top_event_cv%NOTFOUND;
      APPEND_ROW(
        FORMAT_ROW(
          COLUMN_CONTENT_ARRAY(
            top_event_pval_rec.EVENT,
            TO_CHAR(ROUND(100 * top_event_pval_rec.EVENT_CNT/NUM_EVENTS,2), DIG_2_FM) || '%',
            TO_CHAR(ROUND(100 * top_event_pval_rec.SAMPLE_CNT/DUR_ELAPSED,3), DIG_3_FM) || '%',
            '"' || TO_CHAR(top_event_pval_rec.P1) || '","' || TO_CHAR(top_event_pval_rec.P2) || '","' || TO_CHAR(top_event_pval_rec.P3) || '"',
            NVL(top_event_pval_rec.P1TEXT, ' '),
            NVL(top_event_pval_rec.P2TEXT, ' '),
            NVL(top_event_pval_rec.P3TEXT, ' ')
          ),
          column_widths, ' ', '|'
        )
      );
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));


  -- Not implemented yet
  -- APPEND_ROW(' ');
  -- APPEND_ROW('## Top Service/Module:');
  -- column_widths := COLUMN_WIDTH_ARRAY(40, 40, 12, 40, 12);
  -- column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-');
  -- APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  -- column_content := COLUMN_CONTENT_ARRAY('Service', 'Module', '% Activity', 'Action', '% Action');
  -- APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  -- column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-');
  -- APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  -- DYN_SQL := 'SELECT * FROM (SELECT MODULE, ACTION, COUNT(1) SAMPLE_CNT ' ||
  --            'FROM   (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' ||
  --            'GROUP BY MODULE, ROLLUP(ACTION) ORDER BY MODULE, SAMPLE_CNT DESC) ' ||
  --            'WHERE SAMPLE_CNT / :num_samples_param > -0.01';
  -- OPEN top_event_cv FOR DYN_SQL
  -- USING   L_BTIME, L_ETIME,
  --         L_BTIME, L_ETIME,
  --         NULL_CHAR, NULL_CHAR,
  --         NULL_CHAR, NULL_CHAR,
  --         NULL_CHAR, NULL_CHAR,
  --         NULL_CHAR, NULL_CHAR,
  --         NULL_CHAR, NULL_CHAR,
  --         NULL_CHAR, NULL_CHAR,
  --         NUM_SAMPLES;
  -- LOOP
  --     FETCH top_event_cv INTO top_appinfo_rec;
  --     EXIT WHEN top_event_cv%NOTFOUND;
  --     APPEND_ROW(
  --       FORMAT_ROW(
  --         COLUMN_CONTENT_ARRAY(
  --           '*',
  --           NVL(top_appinfo_rec.MODULE, ' '),
  --           TO_CHAR(ROUND(100 * top_appinfo_rec.SAMPLE_CNT/DUR_ELAPSED, 3), DIG_3_FM) || '%',
  --           NVL(top_appinfo_rec.ACTION, ' '),
  --           TO_CHAR(ROUND(100 * top_appinfo_rec.SAMPLE_CNT/NUM_SAMPLES,2), DIG_2_FM) || '%'
  --         ),
  --         column_widths, ' ', '|'
  --       )
  --     );
  -- END LOOP;
  -- CLOSE top_event_cv;
  -- column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-');
  -- APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));



  APPEND_ROW(' ');
  APPEND_ROW('## Top Phase of Execution:');
  column_widths := COLUMN_WIDTH_ARRAY(40, 12, 14, 40);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Phase of Execution', '% Activity', 'Sample Count', 'Avg Active Sessions');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT * FROM (SELECT ' ||
             ' SUM(CASE IN_PARSE WHEN ''N'' THEN 0 ELSE 1 END) IN_PARSE, ' ||
             ' SUM(CASE IN_PL_PARSE WHEN ''N'' THEN 0 ELSE 1 END) IN_PL_PARSE, ' ||
             ' SUM(CASE IN_PLAN_CACHE WHEN ''N'' THEN 0 ELSE 1 END) IN_PLAN_CACHE, ' ||
             ' SUM(CASE IN_SQL_OPTIMIZE WHEN ''N'' THEN 0 ELSE 1 END) IN_SQL_OPTIMIZE, ' ||
             ' SUM(CASE IN_SQL_EXECUTION WHEN ''N'' THEN 0 ELSE 1 END) IN_SQL_EXECUTION, ' ||
             ' SUM(CASE IN_PX_EXECUTION WHEN ''N'' THEN 0 ELSE 1 END) IN_PX_EXECUTION, ' ||
             ' SUM(CASE IN_SEQUENCE_LOAD WHEN ''N'' THEN 0 ELSE 1 END) IN_SEQUENCE_LOAD ' ||
             'FROM   (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ) phases ' ||
             ' unpivot ' ||
             ' (' ||
             '  SAMPLES_CNT FOR EXECUTION_PHASE IN (IN_PARSE, IN_PL_PARSE, IN_PLAN_CACHE, IN_SQL_OPTIMIZE, IN_SQL_EXECUTION,IN_PX_EXECUTION, IN_SEQUENCE_LOAD )' ||
             ' ) ORDER BY SAMPLES_CNT DESC';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
      FETCH top_event_cv INTO top_phase_rec;
      EXIT WHEN top_event_cv%NOTFOUND;
      APPEND_ROW(
        FORMAT_ROW(
          COLUMN_CONTENT_ARRAY(
            top_phase_rec.EXECUTION_PHASE,
            TO_CHAR(top_phase_rec.SAMPLE_CNT),
            TO_CHAR(ROUND(100 * top_phase_rec.SAMPLE_CNT/NUM_SAMPLES, 3), DIG_3_FM) || '%',
            TO_CHAR(ROUND(top_phase_rec.SAMPLE_CNT/DUR_ELAPSED,2), DIG_2_FM)
          ),
          column_widths, ' ', '|'
        )
      );
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));



  APPEND_ROW(' ');
  APPEND_ROW('## Top SQL with Top Events');
  APPEND_ROW(' - All events included.');
  APPEND_ROW(' - Empty ''SQL Text'' if it is PL/SQL query');
  column_widths := COLUMN_WIDTH_ARRAY(40, 12, 25, 40, 12, 60);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('SQL ID', 'PLAN ID', 'Sampled # of Executions', 'Event', '% Event', 'SQL Text');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT SQL_ID, PLAN_ID, EVENT_CNT, EVENT, QUERY_SQL FROM (SELECT ash.*, SUBSTR(TRIM(REPLACE(pc.QUERY_SQL, CHR(10), '''')), 0, 55) QUERY_SQL ' ||
             'FROM (SELECT SQL_ID, PLAN_ID, SUM(' || FILTER_EVENT_STR || ') EVENT_CNT, EVENT FROM (' ||
                DBMS_ASH_INTERNAL.ASH_VIEW_SQL ||
              ') top_event GROUP BY SQL_ID, PLAN_ID, EVENT) ash ' ||
             'LEFT JOIN SYS.GV$OB_PLAN_CACHE_PLAN_STAT pc ON ash.sql_id = pc.sql_id AND ash.plan_id = pc.plan_id ORDER BY EVENT_CNT DESC) v1 WHERE ROWNUM < 100';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
    FETCH top_event_cv INTO top_sql_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          top_sql_rec.SQL_ID,
          TO_CHAR(top_sql_rec.PLAN_ID),
          TO_CHAR(top_sql_rec.EVENT_CNT),
          top_sql_rec.EVENT,
          TO_CHAR(ROUND(100 * top_sql_rec.EVENT_CNT/NUM_EVENTS, 2), DIG_2_FM) || '%',
          NVL(top_sql_rec.QUERY_SQL, ' ')
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));




  APPEND_ROW(' ');
  APPEND_ROW('## Top SQL with Top Blocking Events');
  APPEND_ROW(' - Empty result if no event other than On CPU sampled');
  APPEND_ROW(' - Empty ''SQL Text'' if it is PL/SQL query');
  column_widths := COLUMN_WIDTH_ARRAY(40, 12, 25, 40, 12, 60);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('SQL ID', 'PLAN ID', 'Sampled # of Executions', 'Event', '% Event', 'SQL Text');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT SQL_ID, PLAN_ID, EVENT_CNT, EVENT, QUERY_SQL ' ||
             'FROM (SELECT ash.*, SUBSTR(REPLACE(pc.QUERY_SQL, CHR(10), '' ''), 0, 55) QUERY_SQL ' ||
             ' FROM (SELECT SQL_ID, PLAN_ID, SUM(' || FILTER_EVENT_STR || ') EVENT_CNT, EVENT FROM (' ||
                DBMS_ASH_INTERNAL.ASH_VIEW_SQL ||
             ' ) top_event WHERE wait_class_id != 100 GROUP BY SQL_ID, PLAN_ID, EVENT) ash ' ||
             'LEFT JOIN GV$OB_PLAN_CACHE_PLAN_STAT pc ON ash.sql_id = pc.sql_id AND ash.plan_id = pc.plan_id ORDER BY EVENT_CNT DESC) WHERE ROWNUM < 100';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
    FETCH top_event_cv INTO top_sql_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          top_sql_rec.SQL_ID,
          TO_CHAR(top_sql_rec.PLAN_ID),
          TO_CHAR(top_sql_rec.EVENT_CNT),
          top_sql_rec.EVENT,
          TO_CHAR(ROUND(100 * top_sql_rec.EVENT_CNT/NUM_EVENTS, 2), DIG_2_FM) || '%',
          NVL(top_sql_rec.QUERY_SQL, ' ')
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));

  -- complete List of SQL Text
  APPEND_ROW(' ');
  APPEND_ROW('## Complete List of SQL Text');
  DYN_SQL := 'SELECT SQL_ID, QUERY_SQL FROM (SELECT pc.SQL_ID SQL_ID, pc.QUERY_SQL QUERY_SQL ' ||
             'FROM (SELECT SQL_ID, PLAN_ID, COUNT(1) EVENT_CNT FROM (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event GROUP BY SQL_ID, PLAN_ID, EVENT) ash ' ||
             'LEFT JOIN GV$OB_PLAN_CACHE_PLAN_STAT pc ON ash.sql_id = pc.sql_id AND ash.plan_id = pc.plan_id ORDER BY EVENT_CNT DESC) WHERE QUERY_SQL IS NOT NULL AND ROWNUM < 100';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR;
  LOOP
    FETCH top_event_cv INTO complete_sql_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW('  SQL ID: ' || NVL(complete_sql_rec.SQL_ID, ' '));
    APPEND_ROW('SQL Text: ' || NVL(SUBSTR(complete_sql_rec.QUERY_SQL, 0, 4000), ' '));
    APPEND_ROW('');
  END LOOP;
  CLOSE top_event_cv;


  APPEND_ROW(' ');
  APPEND_ROW('## Top Sessions:');
  APPEND_ROW(' - ''# Samples Active'' shows the number of ASH samples in which the session was found waiting for that particular event. The percentage shown in this column is calculated with respect to wall time.');
  column_widths := COLUMN_WIDTH_ARRAY(20, 22, 40, 12, 12, 20, '20');
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Sid', '% Activity', 'Event', 'Event Count', '% Event', 'User', '# Samples Active');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT  SESSION_ID, EVENT, EVENT_CNT, SAMPLE_CNT, USERNAME USER_NAME ' ||
             ' FROM (SELECT * FROM (SELECT SESSION_ID, USER_ID, EVENT, SUM(' || FILTER_EVENT_STR || ') EVENT_CNT, COUNT(1) SAMPLE_CNT FROM (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' ||
             ' GROUP BY SESSION_ID, USER_ID, EVENT HAVING COUNT(1) / :num_samples > 0.005 ORDER BY SAMPLE_CNT DESC) WHERE ROWNUM < 100) ash ' ||
             ' LEFT JOIN SYS.ALL_USERS u ON u.USERID = ash.USER_ID';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR, NUM_SAMPLES;
  LOOP
    FETCH top_event_cv INTO top_sess_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          TO_CHAR(top_sess_rec.SESSION_ID),
          TO_CHAR(ROUND(100 * top_sess_rec.SAMPLE_CNT/NUM_SAMPLES, 2), DIG_2_FM) || '%',
          top_sess_rec.EVENT,
          TO_CHAR(top_sess_rec.EVENT_CNT),
          TO_CHAR(ROUND(100 * top_sess_rec.EVENT_CNT/NUM_EVENTS, 2), DIG_2_FM) || '%',
          NVL(top_sess_rec.USER_NAME, ' '),
          TO_CHAR(top_sess_rec.EVENT_CNT) || '/' || TO_CHAR(DUR_ELAPSED) || '[' || TO_CHAR(ROUND(100*top_sess_rec.EVENT_CNT/DUR_ELAPSED, 2), DIG_2_FM) || '%]'
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));




  APPEND_ROW(' ');
  APPEND_ROW('## Top Blocking Sessions:');
  APPEND_ROW(' - Blocking session activity percentages are calculated with respect to waits on latches and locks only.');
  APPEND_ROW(' - ''# Samples Active'' shows the number of ASH samples in which the blocking session was found active.');
  column_widths := COLUMN_WIDTH_ARRAY(20, 22, 40, 12, 12, 20, '20');
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Blocking Sid', '% Activity', 'Event Caused', 'Event Count', '% Event', 'User', '# Samples Active');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT  SESSION_ID, EVENT, EVENT_CNT, SAMPLE_CNT, USERNAME USER_NAME ' ||
             ' FROM (SELECT * FROM (SELECT SESSION_ID, USER_ID, EVENT, SUM(' || FILTER_EVENT_STR || ') EVENT_CNT, COUNT(1) SAMPLE_CNT FROM (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' ||
             ' WHERE wait_class_id != 100 GROUP BY SESSION_ID, USER_ID, EVENT HAVING COUNT(1) / :num_samples > 0.005 ORDER BY SAMPLE_CNT DESC) WHERE ROWNUM < 100) ash ' ||
             ' LEFT JOIN SYS.ALL_USERS u ON u.USERID = ash.USER_ID';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR, NUM_SAMPLES;
  LOOP
    FETCH top_event_cv INTO top_sess_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    DBMS_OUTPUT.PUT_LINE(TO_CHAR(top_sess_rec.EVENT_CNT));
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          TO_CHAR(top_sess_rec.SESSION_ID),
          TO_CHAR(ROUND(100 * top_sess_rec.SAMPLE_CNT/NUM_SAMPLES, 2), DIG_2_FM) || '%',
          top_sess_rec.EVENT,
          TO_CHAR(top_sess_rec.EVENT_CNT),
          TO_CHAR(ROUND(100 * top_sess_rec.EVENT_CNT/NUM_EVENTS, 2), DIG_2_FM) || '%',
          top_sess_rec.USER_NAME,
          TO_CHAR(top_sess_rec.EVENT_CNT) || '/' || TO_CHAR(DUR_ELAPSED) || '[' || TO_CHAR(ROUND(100*top_sess_rec.EVENT_CNT/DUR_ELAPSED, 2), DIG_2_FM) || '%]'
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-', '-', '-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));



  APPEND_ROW(' ');
  APPEND_ROW('## Top latches:');
  column_widths := COLUMN_WIDTH_ARRAY(40, 20, 20);
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  column_content := COLUMN_CONTENT_ARRAY('Latch', 'Sampled Count', '% Activity');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, ' ', '|'));
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));
  DYN_SQL := 'SELECT * FROM (SELECT EVENT, COUNT(1) SAMPLE_CNT FROM (' || DBMS_ASH_INTERNAL.ASH_VIEW_SQL || ') top_event ' ||
             ' WHERE wait_class_id = 104 AND SUBSTR(event, 0, 6) = ''latch:'' GROUP BY EVENT HAVING COUNT(1) / :num_samples > 0.005 ORDER BY SAMPLE_CNT DESC) WHERE ROWNUM < 100';
  OPEN top_event_cv FOR DYN_SQL
  USING   L_BTIME, L_ETIME,
          L_BTIME, L_ETIME,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR,
          NULL_CHAR, NULL_CHAR, NUM_SAMPLES;
  LOOP
    FETCH top_event_cv INTO top_latch_rec;
    EXIT WHEN top_event_cv%NOTFOUND;
    APPEND_ROW(FORMAT_ROW(COLUMN_CONTENT_ARRAY(
          TO_CHAR(top_latch_rec.EVENT),
          TO_CHAR(top_latch_rec.SAMPLE_CNT),
          TO_CHAR(ROUND(100 * top_latch_rec.SAMPLE_CNT/NUM_SAMPLES, 2), DIG_2_FM) || '%'
    ), column_widths, ' ', '|'));
  END LOOP;
  CLOSE top_event_cv;
  column_content := COLUMN_CONTENT_ARRAY('-', '-', '-');
  APPEND_ROW(FORMAT_ROW(column_content, column_widths, '-', '+'));


  return RPT_ROWS;

END ASH_REPORT_TEXT;



END dbms_workload_repository;
