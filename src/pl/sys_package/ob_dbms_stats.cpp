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

#define USING_LOG_PREFIX SQL_ENG
#include "pl/sys_package/ob_dbms_stats.h"
#include "share/stat/ob_dbms_stats_executor.h"
#include "share/schema/ob_part_mgr_util.h"
#include "sql/parser/ob_parser.h"
#include "share/ob_rpc_struct.h"
#include "share/stat/ob_dbms_stats_utils.h"
#include "share/stat/ob_opt_stat_monitor_manager.h"
#include "share/stat/ob_opt_stat_manager.h"
#include "share/stat/ob_dbms_stats_export_import.h"
#include "share/stat/ob_dbms_stats_lock_unlock.h"
#include "share/stat/ob_basic_stats_estimator.h"
#include "lib/worker.h"
#include "share/stat/ob_dbms_stats_history_manager.h"
#include "share/stat/ob_index_stats_estimator.h"
#include "lib/timezone/ob_time_convert.h"
#include "sql/das/ob_das_location_router.h"
#include "sql/ob_sql_utils.h"
#include "storage/ob_locality_manager.h"
#include "share/stat/ob_opt_stat_gather_stat.h"
#include "sql/engine/expr/ob_expr_uuid.h"

namespace oceanbase
{
using namespace sql;
using namespace common;
using namespace share::schema;
namespace pl {
/**
 * @brief ObDbmsStat::gather_table_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. tabname       VARCHAR2,
 *      2. partname      VARCHAR2    DEFAULT NULL,
 *      3. estimate_percent NUMBER   DEFAULT to_estimate_percent_type(get_param('ESTIMATE_PERCENT')),
 *      4. block_sample  BOOLEAN     DEFAULT FALSE,
 *      5. method_opt    VARCHAR2    DEFAULT get_param('METHOD_OPT'),
 *      6. degree        NUMBER      DEFAULT to_degree_type(get_param('DEGREE')),
 *      7. granularity   VARCHAR2    DEFAULT get_param('granularity'),
 *      8. cascade       BOOLEAN     DEFAULT to-cascade_type(get_param('CASCADE')),
 *      9. stattab       VARCHAR2    DEFAULT NULL,
 *      10. statid        VARCHAR2    DEFAULT NULL,
 *      11. statown       VARCHAR2    DEFAULT NULL,
 *      12. no_invalidate BOOLEAN     DEFAULT to_no_invalidate_type(get_param('NO_INVALIDATE')),
 *      13. stattype      VARCHAR2    DEFAULT 'DATA',
 *      14. force         BOOLEAN     DEFAULT false,
 *      15. context       DBMS_STATS.CONTEXT DEFAULT NULL,
 *      16. options       VARCHAR2    DEFAULT 'GATHER'
 * @param result
 * @return
 */
int ObDbmsStats::gather_table_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  bool is_all_fast_gather = false;
  ObSEArray<int64_t, 4> no_gather_index_ids;
  ObOptStatTaskInfo task_info;
  int64_t task_cnt = 1;
  int64_t start_time = ObTimeUtility::current_time();
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_ISNULL(ctx.get_my_session()) || OB_ISNULL(ctx.get_task_executor_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(ctx.get_my_session()), K(ctx.get_task_executor_ctx()));
  } else if (OB_FAIL(init_gather_task_info(ctx, ObOptStatGatherType::MANUAL_GATHER, start_time, task_cnt, task_info))) {
    LOG_WARN("failed to init gather task info", K(ret));
  } else {
    ObOptStatGatherStat gather_stat(task_info);
    ObOptStatGatherStatList::instance().push(gather_stat);
    ObOptStatRunningMonitor running_monitor(ctx.get_allocator(), start_time, stat_param.allocator_->used(), gather_stat);
    if (OB_FAIL(parse_table_part_info(ctx,
                                      params.at(0),
                                      params.at(1),
                                      params.at(2),
                                      stat_param))) {
      LOG_WARN("failed to parse owner", K(ret));
    } else if (OB_FAIL(parse_gather_stat_options(ctx,
                                                params.at(3),
                                                params.at(4),
                                                params.at(5),
                                                params.at(6),
                                                params.at(7),
                                                params.at(8),
                                                params.at(12),
                                                params.at(14),
                                                stat_param))) {
      LOG_WARN("failed to parse stat optitions", K(ret));
    } else if (OB_FAIL(running_monitor.add_table_info(stat_param))) {
      LOG_WARN("failed to add table info", K(ret));
    } else if (stat_param.force_ &&
              OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {// Note:
      LOG_WARN("failed fill stat locked", K(ret));
    } else if (!stat_param.force_ &&
              OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed check stat locked", K(ret));
    } else if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx, false, true))) {
      LOG_WARN("failed to do flush database monitoring info", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExecutor::gather_table_stats(ctx, stat_param))) {// Note:
      LOG_WARN("failed to gather table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(),
                                         stat_param,
                                         &running_monitor))) {
      LOG_WARN("failed to update stat cache", K(ret));
    } else if (!need_gather_index_stats(stat_param)) {
      //not gather virtual table/external table index.
    } else if (stat_param.cascade_ &&
              OB_FAIL(fast_gather_index_stats(ctx, stat_param,
                                              is_all_fast_gather, no_gather_index_ids))) {
      LOG_WARN("failed to fast gather index stats", K(ret));
    } else if (stat_param.cascade_ && !is_all_fast_gather &&
              OB_FAIL(gather_table_index_stats(ctx, stat_param, no_gather_index_ids))) {
      LOG_WARN("failed to gather table index stats", K(ret));
    } else {
      LOG_TRACE("Succeed to gather table stats", K(stat_param));
    }
    running_monitor.set_monitor_result(ret, ObTimeUtility::current_time(), stat_param.allocator_->used());
    task_info.task_end_time_ = ObTimeUtility::current_time();
    task_info.ret_code_ = ret;
    task_info.failed_count_ = ret == OB_SUCCESS ? 0 : 1;
    ObOptStatManager::get_instance().update_opt_stat_task_stat(task_info);
    ObOptStatManager::get_instance().update_opt_stat_gather_stat(gather_stat);
    ObOptStatGatherStatList::instance().remove(gather_stat);
  }
  return ret;
}

/**
 * @brief ObDbmsStat::gather_schema_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. estimate_percent NUMBER   DEFAULT to_estimate_percent_type(get_param('ESTIMATE_PERCENT')),
 *      2. block_sample  BOOLEAN     DEFAULT FALSE,
 *      3. method_opt    VARCHAR2    DEFAULT get_param('METHOD_OPT'),
 *      4. degree        NUMBER      DEFAULT to_degree_type(get_param('DEGREE')),
 *      5. granularity   VARCHAR2    DEFAULT get_param('granularity'),
 *      6. cascade       BOOLEAN     DEFAULT to-cascade_type(get_param('CASCADE')),
 *      7. stattab       VARCHAR2    DEFAULT NULL,
 *      8. statid        VARCHAR2    DEFAULT NULL,
 *      9. statown       VARCHAR2    DEFAULT NULL,
 *      10. no_invalidate BOOLEAN     DEFAULT to_no_invalidate_type(get_param('NO_INVALIDATE')),
 *      11. stattype      VARCHAR2    DEFAULT 'DATA',
 *      12. force         BOOLEAN     DEFAULT false
 * @param result
 * @return
 */
int ObDbmsStats::gather_schema_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam global_param;
  global_param.allocator_ = &ctx.get_allocator();
  ObSEArray<uint64_t, 4> table_ids;
  ObOptStatTaskInfo task_info;
  int64_t start_time = ObTimeUtility::current_time();
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_ISNULL(ctx.get_my_session()) || OB_ISNULL(ctx.get_task_executor_ctx())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(ctx.get_my_session()), K(ctx.get_task_executor_ctx()));
  } else if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx, false, true))) {
    LOG_WARN("failed to do flush database monitoring info", K(ret));
  } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
    LOG_WARN("failed to get all table ids in database", K(ret));
  } else if (table_ids.empty()) {
    //do nothing
  } else if (OB_FAIL(init_gather_task_info(ctx, ObOptStatGatherType::MANUAL_GATHER, start_time, table_ids.count(), task_info))) {
    LOG_WARN("failed to init gather task info", K(ret));
  } else {
    int64_t i = 0;
    for (; OB_SUCC(ret) && i < table_ids.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = global_param.db_id_;
      stat_table.table_id_ = table_ids.at(i);
      ObTableStatParam stat_param = global_param;
      ObArenaAllocator tmp_alloc("OptStatGather", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      stat_param.allocator_ = &tmp_alloc;//use the temp allocator to free memory after gather stats.
      bool is_all_fast_gather = false;
      ObSEArray<int64_t, 4> no_gather_index_ids;
      int64_t start_time = ObTimeUtility::current_time();
      ObOptStatGatherStat gather_stat(task_info);
      ObOptStatGatherStatList::instance().push(gather_stat);
      ObOptStatRunningMonitor running_monitor(ctx.get_allocator(), start_time, stat_param.allocator_->used(), gather_stat);
      if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else if (OB_FAIL(parse_gather_stat_options(ctx,
                                                  params.at(1),
                                                  params.at(2),
                                                  params.at(3),
                                                  params.at(4),
                                                  params.at(5),
                                                  params.at(6),
                                                  params.at(10),
                                                  params.at(12),
                                                  stat_param))) {
        LOG_WARN("failed to parse stat optitions", K(ret));
      } else if (OB_FAIL(running_monitor.add_table_info(stat_param))) {
        LOG_WARN("failed to add table info", K(ret));
      } else if (stat_param.force_ &&
                OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
        LOG_WARN("failed fill stat locked", K(ret));
      } else if (!stat_param.force_ &&
                OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
        if (OB_ERR_DBMS_STATS_PL == ret) {
          // all table/partition locked, just skip
          ret = OB_SUCCESS;
          LOG_TRACE("table locked, just skip", K(stat_param));
        } else {
          LOG_WARN("failed check stat locked", K(ret));
        }
      } else if (share::schema::ObTableType::EXTERNAL_TABLE == stat_param.ref_table_type_) {
        // not allow gather external table in schema scope
      } else if (OB_FAIL(ObDbmsStatsExecutor::gather_table_stats(ctx, stat_param))) {// Note:
        LOG_WARN("failed to gather table stats", K(ret));
      } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(),
                                            stat_param,
                                            &running_monitor))) {
        LOG_WARN("failed to update stat cache", K(ret));
      } else if (is_virtual_table(stat_param.table_id_)) {//not gather virtual table index.
        //do nothing
      } else if (stat_param.cascade_ &&
                OB_FAIL(fast_gather_index_stats(ctx, stat_param,
                                                is_all_fast_gather, no_gather_index_ids))) {
        LOG_WARN("failed to fast gather index stats", K(ret));
      } else if (stat_param.cascade_ && !is_all_fast_gather &&
                OB_FAIL(gather_table_index_stats(ctx, stat_param, no_gather_index_ids))) {
        LOG_WARN("failed to gather table index stats", K(ret));
      } else {
        LOG_TRACE("Succeed to gather table stats", K(stat_param), K(running_monitor));
      }
      running_monitor.set_monitor_result(ret, ObTimeUtility::current_time(), stat_param.allocator_->used());
      ObOptStatManager::get_instance().update_opt_stat_gather_stat(gather_stat);
      ObOptStatGatherStatList::instance().remove(gather_stat);
      task_info.completed_table_count_ ++;
    }
    task_info.task_end_time_ = ObTimeUtility::current_time();
    task_info.ret_code_ = ret;
    task_info.failed_count_ = ret == OB_SUCCESS ? 0 : table_ids.count() - i + 1;
    ObOptStatManager::get_instance().update_opt_stat_task_stat(task_info);
  }
  return ret;
}

/**
 * @brief ObDbmsStat::gather_index_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. indname       VARCHAR2,
 *      2. partname      VARCHAR2 DEFAULT NULL,
 *      3. estimate_percent NUMBER DEFAULT AUTO_SAMPLE_SIZE,
 *      4. stattab       VARCHAR2    DEFAULT NULL,
 *      5. statid        VARCHAR2    DEFAULT NULL,
 *      6. statown       VARCHAR2    DEFAULT NULL,
 *      7. degree        NUMBER      DEFAULT to_degree_type(get_param('DEGREE')),
 *      8  granularity   VARCHAR2    DEFAULT get_param('granularity'),
 *      9. no_invalidate BOOLEAN     DEFAULT to_no_invalidate_type(get_param('NO_INVALIDATE')),
 *      10.force         BOOLEAN     DEFAULT false,
 *      11.tabname       VARCHAR2    DEFAULT NULL(for mysql mode only)
 * @param result
 * @return
 */
int ObDbmsStats::gather_index_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam ind_stat_param;
  ind_stat_param.is_index_stat_ = true;
  ind_stat_param.allocator_ = &ctx.get_allocator();
  ObObjParam empty_sample;
  empty_sample.set_null();
  ObObjParam empty_method_opt;
  empty_method_opt.set_null();
  ObObjParam empty_cascade;
  empty_cascade.set_null();
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode() && !params.at(11).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name shouldn't be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL,"table name shouldn't be specified in gather index stats");
  } else if (lib::is_mysql_mode() && params.at(11).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name should be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "table name should be specified in gather index stats");
  } else if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx, false, true))) {
    LOG_WARN("failed to do flush database monitoring info", K(ret));
  } else if (OB_FAIL(parse_index_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           params.at(11),
                                           ind_stat_param))) {
    LOG_WARN("failed to parse index part info", K(ret));
  } else if (OB_FAIL(parse_gather_stat_options(ctx,
                                               params.at(3),
                                               empty_sample,
                                               empty_method_opt,
                                               params.at(7),
                                               params.at(8),
                                               empty_cascade,
                                               params.at(9),
                                               params.at(10),
                                               ind_stat_param))) {
    LOG_WARN("failed to parse stat optitions", K(ret));
  } else if (ObDbmsStatsUtils::is_virtual_index_table(ind_stat_param.table_id_)) {//not gather virtual table index.
    //do nothing
  } else if (ind_stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, ind_stat_param))) {
    LOG_WARN("failed fill stat locked", K(ret));
  } else if (!ind_stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, ind_stat_param))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExecutor::gather_index_stats(ctx, ind_stat_param))) {// Note:
    LOG_WARN("failed to gather table stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), ind_stat_param))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to gather index stats", K(ind_stat_param));
  }
  return ret;
}

int ObDbmsStats::gather_table_index_stats(ObExecContext &ctx,
                                          const ObTableStatParam &data_param,
                                          ObIArray<int64_t> &no_gather_index_ids)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  int64_t start_time = ObTimeUtility::current_time();
  for (int64_t i = 0; OB_SUCC(ret) && i < no_gather_index_ids.count(); ++i) {
    StatTable stat_table;
    stat_table.database_id_ = data_param.db_id_;
    stat_table.table_id_ = no_gather_index_ids.at(i);
    ObTableStatParam index_param;
    index_param.assign_common_property(data_param);
    const share::schema::ObTableSchema *index_schema = NULL;
    if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
      LOG_WARN("failed to parse table part info", K(ret));
    } else if (OB_ISNULL(schema_guard) ||
                OB_FAIL(schema_guard->get_table_schema(
                        ctx.get_my_session()->get_effective_tenant_id(),
                        stat_table.table_id_, index_schema)) ||
                OB_ISNULL(index_schema)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to get index schema", K(ret), K(stat_table));
    } else if (index_schema->is_global_index_table()) {
      index_param.is_global_index_ = true;
    }
    if (OB_SUCC(ret)) {
      index_param.is_index_stat_ = true;
      index_param.global_stat_param_ = data_param.global_stat_param_;
      index_param.part_stat_param_.assign_without_part_type(data_param.part_stat_param_);
      index_param.subpart_stat_param_.assign_without_part_type(data_param.subpart_stat_param_);
      index_param.data_table_name_ = data_param.tab_name_;
      if (index_param.force_ &&
          OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, index_param))) {
        LOG_WARN("failed fill stat locked", K(ret));
      } else if (!index_param.force_ &&
                  OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, index_param))) {
        LOG_WARN("failed check stat locked", K(ret));
        //refresh duration time
      } else if (OB_FAIL(ObDbmsStatsUtils::get_valid_duration_time(start_time,
                                                                   data_param.duration_time_,
                                                                   index_param.duration_time_))) {
        LOG_WARN("failed to get valid duration time", K(ret));
      } else if (OB_FAIL(ObDbmsStatsExecutor::gather_index_stats(ctx, index_param))) {// Note:
        LOG_WARN("failed to gather table stats", K(ret));
      } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_param))) {
        LOG_WARN("failed to update stat cache", K(ret));
      } else {
        LOG_TRACE("Succeed to gather index stats", K(data_param), K(index_param));
      }
    }
  }
  return ret;
}

//use existing statistics for index statistics collection
int ObDbmsStats::fast_gather_index_stats(ObExecContext &ctx,
                                         const ObTableStatParam &data_param,
                                         bool &is_all_fast_gather,
                                         ObIArray<int64_t> &no_gather_index_ids)
{
  int ret = OB_SUCCESS;
  is_all_fast_gather = true;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  if (OB_FAIL(get_table_index_infos(ctx, data_param.table_id_, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = data_param.db_id_;
      stat_table.table_id_ = simple_index_infos.at(i).table_id_;
      ObTableStatParam index_param;
      index_param.is_index_stat_ = true;
      index_param.assign_common_property(data_param);
      bool is_fast_gather = true;
      const share::schema::ObTableSchema *index_schema = NULL;
      if (simple_index_infos.at(i).table_id_ == data_param.table_id_) {
        //do nothing, remove primary table
      } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else if (is_func_index(index_param)) {//func index can't fast gather
        if (OB_FAIL(no_gather_index_ids.push_back(index_param.table_id_))) {
          LOG_WARN("failed to push back table id", K(ret));
        } else {
          is_fast_gather = false;
          is_all_fast_gather &= is_fast_gather;
          LOG_TRACE("can't fast gather index stat, because the index is func index.", K(data_param),
                                                            K(index_param), K(no_gather_index_ids));
        }
      } else if (OB_ISNULL(schema_guard) ||
                 OB_FAIL(schema_guard->get_table_schema(
                         ctx.get_my_session()->get_effective_tenant_id(),
                         stat_table.table_id_, index_schema)) ||
                 OB_ISNULL(index_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("failed to get index schema", K(ret), K(stat_table));
      //glboal index can't reuse the partition data in fast gather index
      } else if (index_schema->is_global_index_table()) {
        index_param.is_global_index_ = true;
        index_param.global_stat_param_ = data_param.global_stat_param_;
        index_param.global_stat_param_.gather_approx_ = false;
        index_param.part_stat_param_.reset_gather_stat();
        index_param.subpart_stat_param_.reset_gather_stat();
      //local index the partition is same as data table
      } else {
        index_param.global_stat_param_ = data_param.global_stat_param_;
        index_param.part_stat_param_ = data_param.part_stat_param_;
        index_param.subpart_stat_param_ = data_param.subpart_stat_param_;
      }
      if (OB_SUCC(ret) && is_fast_gather) {
        if (OB_FAIL(ObIndexStatsEstimator::fast_gather_index_stats(ctx, data_param,
                                                                   index_param, is_fast_gather))) {
          LOG_WARN("failed to fast gather index stats", K(ret));
        } else if (!is_fast_gather) {
          if (OB_FAIL(no_gather_index_ids.push_back(index_param.table_id_))) {
            LOG_WARN("failed to push back table id", K(ret));
          } else {
            is_all_fast_gather &= is_fast_gather;
            LOG_TRACE("can't fast gather index stats", K(data_param), K(index_param));
          }
        } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_param))) {
          LOG_WARN("failed to update stat cache", K(ret));
        } else {
          is_all_fast_gather &= is_fast_gather;
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::set_table_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. tabname       VARCHAR2,
 *      2. partname      VARCHAR2    DEFAULT NULL,
 *      3. stattab       VARCHAR2    DEFAULT NULL,
 *      4. statid        VARCHAR2    DEFAULT NULL,
 *      5. numrows       NUMBER      DEFAULT NULL,
 *      6. numblks       NUMBER      DEFAULT NULL,
 *      7. avgrlen       NUMBER      DEFAULT NULL,
 *      8. flags         NUMBER      DEFAULT NULL,
 *      9. statown       VARCHAR2    DEFAULT NULL,
 *      10. no_invalidate BOOLEAN    DEFAULT to_no_invalidate_type(get_param('NO_INVALIDATE')),
 *      11. cachedblk     NUMBER     DEFAULT NULL,
 *      12. cachehit      NUMBER     DEFAULT NULL,
 *      13. force         BOOLEAN    DEFAULT FALSE
 *      14. nummacroblks  NUMBER      DEFAULT NULL,
 *      15. nummicroblks  NUMBER      DEFAULT NULL
 * @param result
 * @return
 */
int ObDbmsStats::set_table_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObSetTableStatParam param;
  param.table_param_.allocator_ = &ctx.get_allocator();
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_set_table_info(ctx,
                                          params.at(0),
                                          params.at(1),
                                          params.at(2),
                                          param.table_param_))) {
    LOG_WARN("failed to parse set table info", K(ret));
  } else if (OB_FAIL(parse_set_table_stat_options(ctx,
                                                  params.at(3),
                                                  params.at(4),
                                                  params.at(5),
                                                  params.at(6),
                                                  params.at(7),
                                                  params.at(8),
                                                  params.at(9),
                                                  params.at(10),
                                                  params.at(11),
                                                  params.at(12),
                                                  params.at(13),
                                                  params.at(14),
                                                  params.at(15),
                                                  param))) {
    LOG_WARN("failed to parse set table stat options", K(ret));
  } else if (param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, param.table_param_))) {
    LOG_WARN("failed fill stat locked", K(ret));
  } else if (!param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, param.table_param_))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExecutor::set_table_stats(ctx, param))) {// Note:
    LOG_WARN("failed to set table stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), param.table_param_))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to set table stat", K(param));
  }
  return ret;
}

/**
 * @brief ObDbmsStat::set_column_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. tabname       VARCHAR2,
 *      2. colname       VARCHAR2,
 *      3. partname      VARCHAR2,   DEFAULT NULL,
 *      4. stattab       VARCHAR2    DEFAULT NULL,
 *      5. statid        VARCHAR2    DEFAULT NULL,
 *      6. distcnt       NUMBER      DEFAULT NULL,
 *      7. density       NUMBER      DEFAULT NULL,
 *      8. nullcnt       NUMBER      DEFAULT NULL,
 *      9. epc           NUMBER   DEFAULT NULL,
 *      10. minval       RAW      DEFAULT NULL,
 *      11. maxval       RAW      DEFAULT NULL,
 *      12. bkvals       NUMARRAY  DEFAULT NULL,
 *      13. novals       NUMARRAY  DEFAULT NULL,
 *      14. chvals       CHARARRAY DEFAULT NULL,
 *      15. eavals       RAWARRAY  DEFAULT NULL,
 *      16. rpcnts       NUMARRAY  DEFAULT NULL,
 *      17. eavs         NUMBER    DEFAULT NULL,
 *      18. avgclen      NUMBER      DEFAULT NULL,
 *      19. flags        NUMBER      DEFAULT NULL,
 *      20. statown      VARCHAR2    DEFAULT NULL,
 *      21. no_invalidate BOOLEAN    DEFAULT to_no_invalidate_type(get_param('NO_INVALIDATE')),
 *      22. force         BOOLEAN     DEFAULT false
 * @param result
 * @return
 */
/** Note:外部接口
 * 功能:
 * 设置column的状态(这里是PL存储过程调用)
 * 调用:
 * ob_pl_interface_pragma.h
*/
int ObDbmsStats::set_column_stats(sql::ObExecContext &ctx,
                                  sql::ParamStore &params,
                                  common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObSetColumnStatParam param;
  param.table_param_.allocator_ = &ctx.get_allocator();
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (params.at(2).is_null() && !params.at(1).is_null()) {
    //do nothing
  } else if (OB_FAIL(parse_set_column_stats(ctx,
                                            params.at(0),
                                            params.at(1),
                                            params.at(2),
                                            params.at(3),
                                            param.table_param_))) {
    LOG_WARN("failed to parse set column stats", K(ret));
  } else if (OB_FAIL(parse_set_column_stats_options(ctx,
                                                    params.at(4),
                                                    params.at(5),
                                                    params.at(6),
                                                    params.at(7),
                                                    params.at(8),
                                                    params.at(18),
                                                    params.at(19),
                                                    params.at(20),
                                                    params.at(21),
                                                    params.at(22),
                                                    param))) {
    LOG_WARN("failed to parse set column stats options", K(ret));
  } else if (OB_FAIL(parse_set_hist_stats_options(ctx,
                                                  params.at(9),
                                                  params.at(10),
                                                  params.at(11),
                                                  params.at(12),
                                                  params.at(13),
                                                  params.at(14),
                                                  params.at(15),
                                                  params.at(16),
                                                  params.at(17),
                                                  param.hist_param_))) {
    LOG_WARN("failed to parse set column stats options", K(ret));
  } else if (param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, param.table_param_))) {
    LOG_WARN("failed fill stat locked", K(ret));
  } else if (!param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, param.table_param_))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExecutor::set_column_stats(ctx, param))) {// Note:
    LOG_WARN("failed to set column stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), param.table_param_))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to set column stats", K(param));
  }
  return ret;
}

/**
 * @brief ObDbmsStat::set_index_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. indname       VARCHAR2,
 *      2. partname      VARCHAR2    DEFAULT NULL,
 *      3. stattab       VARCHAR2    DEFAULT NULL,
 *      4. statid        VARCHAR2    DEFAULT NULL,
 *      5. numrows       NUMBER      DEFAULT NULL,
 *      6. numlblks      NUMBER      DEFAULT NULL,
 *      7. numdist       NUMBER      DEFAULT NULL,
 *      8. avglblk       NUMBER      DEFAULT NULL,
 *      9. avgdblk       NUMBER      DEFAULT NULL,
 *      10.clstfct       NUMBER      DEFAULT NULL,
 *      11.indlevel      NUMBER      DEFAULT NULL,
 *      12.flags         NUMBER      DEFAULT NULL,
 *      13.statown       VARCHAR2    DEFAULT NULL,
 *      14.no_invalidate BOOLEAN     DEFAULT FALSE,
 *      15.guessq        NUMBER      DEFAULT NULL,
 *      16.cachedblk     NUMBER      DEFAULT NULL,
 *      17.cachehit      NUMBER      DEFUALT NULL,
 *      18.force         BOOLEAN     DEFAULT FALSE,
 *      19.avgrlen       NUMBER      DEFUALT NULL,
 *      20.nummacroblks  NUMBER      DEFAULT NULL,
 *      21.nummicroblks  NUMBER      DEFAULT NULL,
 *      22.tabname       VARCHAR2    DEFAULT NULL(for mysql mode only)
 * @param result
 * @return
 */
int ObDbmsStats::set_index_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObSetTableStatParam set_index_param;
  ObTableStatParam index_stat_param;
  index_stat_param.is_index_stat_ = true;
  index_stat_param.allocator_ = &ctx.get_allocator();
  number::ObNumber num_numrows;
  number::ObNumber num_avgrlen;
  number::ObNumber num_nummacroblks;
  number::ObNumber num_nummicroblks;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode() && !params.at(22).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name shouldn't be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL,"table name shouldn't be specified in gather index stats");
  } else if (lib::is_mysql_mode() && params.at(22).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name should be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "table name should be specified in gather index stats");
  } else if (OB_FAIL(parse_index_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           params.at(22),
                                           index_stat_param))) {
    LOG_WARN("failed to parse index part info", K(ret));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_number(num_numrows))) {
    LOG_WARN("failed to get ncachehit", K(ret));
  } else if (!params.at(18).is_null() && OB_FAIL(params.at(18).get_bool(index_stat_param.force_))) {
    LOG_WARN("failed to get force", K(ret));
  } else if (!params.at(19).is_null() && OB_FAIL(params.at(19).get_number(num_avgrlen))) {
    LOG_WARN("failed to get avgrlen", K(ret));
  } else if (!params.at(20).is_null() && OB_FAIL(params.at(20).get_number(num_nummacroblks))) {
    LOG_WARN("failed to get nummacroblks", K(ret));
  } else if (!params.at(21).is_null() && OB_FAIL(params.at(21).get_number(num_nummicroblks))) {
    LOG_WARN("failed to get nummicroblks", K(ret));
  } else if (OB_FAIL(num_numrows.extract_valid_int64_with_trunc(set_index_param.numrows_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_numrows));
  } else if (OB_FAIL(num_avgrlen.extract_valid_int64_with_trunc(set_index_param.avgrlen_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_avgrlen));
  } else if (OB_FAIL(num_nummacroblks.extract_valid_int64_with_trunc(set_index_param.nummacroblks_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_nummacroblks));
  } else if (OB_FAIL(num_nummicroblks.extract_valid_int64_with_trunc(set_index_param.nummicroblks_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_nummicroblks));
  } else {
    decide_modified_part(index_stat_param, false/* cascade_part */);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(set_index_param.table_param_.assign(index_stat_param))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (set_index_param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, set_index_param.table_param_))) {
    LOG_WARN("failed fill stat locked", K(ret));
  } else if (!set_index_param.table_param_.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, set_index_param.table_param_))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExecutor::set_table_stats(ctx, set_index_param))) {// Note:
    LOG_WARN("failed to set table stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(),
                                       set_index_param.table_param_))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to set index stat", K(set_index_param));
  }
  return ret;
}

/**
 * @brief ObDbmsStats::delete_table_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. tabname           VARCHAR2,
 *   2. partname          VARCHAR2 DEFAULT NULL,
 *   3. stattab           VARCHAR2 DEFAULT NULL,
 *   4. statid            VARCHAR2 DEFAULT NULL,
 *   5. cascade_parts     BOOLEAN DEFAULT TRUE,
 *   6. cascade_columns   BOOLEAN DEFAULT TRUE,
 *   7. cascade_indexes   BOOLEAN DEFAULT TRUE,
 *   8. statown           VARCHAR2 DEFAULT NULL,
 *   9. no_invalidate     BOOLEAN DEFAULT FALSE,
 *  10. force             BOOLEAN DEFAULT FALSE
 * @param result
 * @return
 */
int ObDbmsStats::delete_table_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  bool cascade_parts = false;
  bool cascade_columns = false;
  bool cascade_indexes = false;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(cascade_parts))) {
    LOG_WARN("failed to get cascade partition", K(ret));
  } else if (!params.at(6).is_null() && OB_FAIL(params.at(6).get_bool(cascade_columns))) {
    LOG_WARN("failedt oget cascade column", K(ret));
  } else if (!params.at(7).is_null() && OB_FAIL(params.at(7).get_bool(cascade_indexes))) {
    LOG_WARN("failedt oget cascade column", K(ret));
  } else if (!params.at(9).is_null() && OB_FAIL(params.at(9).get_bool(stat_param.no_invalidate_))) {
    LOG_WARN("failed to get no invalidate", K(ret));
  } else if (!params.at(10).is_null() && OB_FAIL(params.at(10).get_bool(stat_param.force_))) {
    LOG_WARN("failed to get no invalidate", K(ret));
  } else if (!cascade_columns) {
    stat_param.column_params_.reset();
  }
  if (OB_SUCC(ret)) {
    decide_modified_part(stat_param, cascade_parts);
    if (!stat_param.part_name_.empty()) {
      cascade_indexes = false;
    }
  }
  if (OB_SUCC(ret)) {
    if (stat_param.force_ &&
        OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed fill stat locked", K(ret));
    } else if (!stat_param.force_ &&
               OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed check stat locked", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExecutor::delete_table_stats(ctx,
                                                               stat_param,
                                                               cascade_columns))) {// Note:
      LOG_WARN("failed to delete table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
      LOG_WARN("failed to update stat cache", K(ret));
    } else if (cascade_indexes && stat_param.part_name_.empty()) {
      if (OB_FAIL(delete_table_index_stats(ctx, stat_param))) {
        LOG_WARN("failed to delete index stats", K(ret));
      } else {/*do nothing*/}
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::delete_column_stats
 * @param ctx
 *   0. ownname          VARCHAR2,
 *   1. tabname          VARCHAR2,
 *   2. colname          VARCHAR2,
 *   3. partname         VARCHAR2 DEFAULT NULL,
 *   4. stattab          VARCHAR2 DEFAULT NULL,
 *   5. statid           VARCHAR2 DEFAULT NULL,
 *   6. cascade_parts    BOOLEAN DEFAULT TRUE,
 *   7. statown          VARCHAR2 DEFAULT NULL,
 *   8. no_invalidate    BOOLEAN DEFAULT FALSE,
 *   9. force            BOOLEAN DEFAULT FALSE,
 *  10. col_stat_type    VARCHAR2 DEFAULT 'ALL'
 * @param params
 * @param result
 * @return
 */
int ObDbmsStats::delete_column_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  ObString col_stat_type("ALL");
  bool cascade_parts = false;
  bool only_histogram = false;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(3),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(parse_column_info(ctx, params.at(2), stat_param))) {
    LOG_WARN("failed to parse export column info", K(ret));
  } else if (!params.at(6).is_null() && OB_FAIL(params.at(6).get_bool(cascade_parts))) {
    LOG_WARN("failed to get cascade partition", K(ret));
  } else if (!params.at(8).is_null() && OB_FAIL(params.at(8).get_bool(stat_param.no_invalidate_))) {
    LOG_WARN("failed to get cascade column", K(ret));
  } else if (!params.at(9).is_null() && OB_FAIL(params.at(9).get_bool(stat_param.force_))) {
    LOG_WARN("failed to get no invalidate", K(ret));
  } else if (!params.at(10).is_null() && OB_FAIL(params.at(10).get_varchar(col_stat_type))) {
    LOG_WARN("failed to get no invalidate", K(ret));
  } else if (!params.at(10).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*stat_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              col_stat_type))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (0 == col_stat_type.case_compare("ALL")) {
    only_histogram = false;
  } else if (0 == col_stat_type.case_compare("HISTOGRAM")) {
    only_histogram = true;
  } else {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("col stat type is invalid", K(ret), K(col_stat_type));
  }

  if (OB_SUCC(ret)) {
    decide_modified_part(stat_param, cascade_parts);
    if (stat_param.force_ &&
        OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed fill stat locked", K(ret));
    } else if (!stat_param.force_ &&
               OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed check stat locked", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExecutor::delete_column_stats(ctx,
                                                                stat_param,
                                                                only_histogram))) {
      LOG_WARN("failed to delete table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
      LOG_WARN("failed to update stat cache", K(ret));
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::delete_schema_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. stattab           VARCHAR2 DEFAULT NULL,
 *   2. statid            VARCHAR2 DEFAULT NULL,
 *   3. statown           VARCHAR2 DEFAULT NULL,
 *   4. no_invalidate     BOOLEAN DEFAULT FALSE,
 *   5. force             BOOLEAN DEFAULT FALSE
 * @param result
 * @return
 */
int ObDbmsStats::delete_schema_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  SMART_VAR(ObTableStatParam, global_param) {
    global_param.allocator_ = &ctx.get_allocator();
    ObSEArray<uint64_t, 4> table_ids;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
      LOG_WARN("failed to get all table ids in database", K(ret));
    } else {
      ObArenaAllocator tmp_alloc("OptStatDelete", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        StatTable stat_table;
        stat_table.database_id_ = global_param.db_id_;
        stat_table.table_id_ = table_ids.at(i);
        ObTableStatParam stat_param = global_param;
        stat_param.allocator_ = &tmp_alloc;//use the temp allocator to free memory after delete stats.
        if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
          LOG_WARN("failed to parse table part info", K(ret));
        } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_bool(stat_param.no_invalidate_))) {
          LOG_WARN("failed to get no invalidate", K(ret));
        } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(stat_param.force_))) {
          LOG_WARN("failed to get no invalidate", K(ret));
        } else {
          stat_param.global_stat_param_.need_modify_ = true;
          stat_param.part_stat_param_.need_modify_ = true;
          stat_param.subpart_stat_param_.need_modify_ = true;
          if (stat_param.force_ &&
              OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
            LOG_WARN("failed to fill stat locked", K(ret));
          } else if (!stat_param.force_ &&
                    OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
            if (OB_ERR_DBMS_STATS_PL == ret) {
              // all table/partition locked, just skip
              ret = OB_SUCCESS;
              LOG_TRACE("table locked, just skip", K(stat_param));
            } else {
              LOG_WARN("failed to check stat locked", K(ret));
            }
          } else if (OB_FAIL(ObDbmsStatsExecutor::delete_table_stats(ctx, stat_param, true))) {// Note:
            LOG_WARN("failed to delete table stats", K(ret));
          } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
            LOG_WARN("failed to update stat cache", K(ret));
          } else if (OB_FAIL(delete_table_index_stats(ctx, stat_param))) {
            LOG_WARN("failed to delete index stats", K(ret));
          } else {
            tmp_alloc.reset();
            LOG_TRACE("Succeed to delete table stats", K(stat_param));
          }
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::delete_index_stats
 * @param ctx
 * @param params
      0.ownname          VARCHAR2,
      1.indname          VARCHAR2,
      2.partname         VARCHAR2 DEFAULT NULL,
      3.stattab          VARCHAR2 DEFAULT NULL,
      4.statid           VARCHAR2 DEFAULT NULL,
      5.cascade_parts    BOOLEAN  DEFAULT TRUE,
      6.statown          VARCHAR2 DEFAULT NULL,
      7.no_invalidate    BOOLEAN  DEFAULT FALSE,
      8.stattype         VARCHAR2 DEFAULT 'ALL',
      9.force            BOOLEAN  DEFAULT FALSE,
      10.tabname         VARCHAR2 DEFAULT NULL(for mysql mode only)
 * @param result
 * @return
 */
int ObDbmsStats::delete_index_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam index_stat_param;
  index_stat_param.is_index_stat_ = true;
  index_stat_param.allocator_ = &ctx.get_allocator();
  bool cascade_parts = false;
  bool only_histogram = false;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode() && !params.at(10).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name shouldn't be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL,"table name shouldn't be specified in gather index stats");
  } else if (lib::is_mysql_mode() && params.at(10).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name should be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "table name should be specified in gather index stats");
  } else if (OB_FAIL(parse_index_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           params.at(10),
                                           index_stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(cascade_parts))) {
    LOG_WARN("failed to get cascade partition", K(ret));
  } else if (!params.at(7).is_null() && OB_FAIL(params.at(7).get_bool(index_stat_param.no_invalidate_))) {
    LOG_WARN("failed to get no invalidate", K(ret));
  } else if (!params.at(9).is_null() && OB_FAIL(params.at(9).get_bool(index_stat_param.force_))) {
    LOG_WARN("failed to get force", K(ret));
  } else {
    decide_modified_part(index_stat_param, cascade_parts);
  }

  if (OB_SUCC(ret)) {
    if (index_stat_param.force_ &&
        OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, index_stat_param))) {
      LOG_WARN("failed fill stat locked", K(ret));
    } else if (!index_stat_param.force_ &&
               OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, index_stat_param))) {
      LOG_WARN("failed check stat locked", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExecutor::delete_table_stats(ctx, index_stat_param, false))) {// Note:
      LOG_WARN("failed to delete table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_stat_param))) {
      LOG_WARN("failed to update stat cache", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::delete_table_index_stats(sql::ObExecContext &ctx,
                                          const ObTableStatParam data_param)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  if (OB_FAIL(get_table_index_infos(ctx, data_param.table_id_, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = data_param.db_id_;
      stat_table.table_id_ = simple_index_infos.at(i).table_id_;
      ObTableStatParam index_param;
      index_param.assign_common_property(data_param);
      if (simple_index_infos.at(i).table_id_ == data_param.table_id_) {
        //do nothing, remove primary table
      } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else {
        index_param.is_index_stat_ = true;
        if (index_param.force_ &&
            OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, index_param))) {
          LOG_WARN("failed fill stat locked", K(ret));
        } else if (!index_param.force_ &&
                   OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, index_param))) {
          LOG_WARN("failed check stat locked", K(ret));
        } else if (OB_FAIL(ObDbmsStatsExecutor::delete_table_stats(ctx, index_param, false))) {// Note:
          LOG_WARN("failed to delete table stats", K(ret));
        } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_param))) {
          LOG_WARN("failed to update stat cache", K(ret));
        } else {/*do nothing*/}
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::create_stat_table
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. stattab          VARCHAR2,
 *      2. tblspace         VARCHAR2 DEFAULT NULL,
 *      3. global_temporary BOOLEAN DEFAULT FALSE,
 * @param result
 * @return
 */
int ObDbmsStats::create_stat_table(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam param;
  param.allocator_ = &ctx.get_allocator();
  ObString tblspace;
  bool is_temp_table = false;
  const share::schema::ObTableSchema *table_schema = NULL;
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(session));
  } else {
    param.tenant_id_ = session->get_effective_tenant_id();
    if (!params.at(0).is_null() && OB_FAIL(params.at(0).get_varchar(param.db_name_))) {
      LOG_WARN("failed to get db_name", K(ret));
    } else if (!params.at(0).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.db_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (!params.at(1).is_null() && OB_FAIL(params.at(1).get_varchar(param.tab_name_))) {
      LOG_WARN("failed to get tab_name", K(ret));
    } else if (!params.at(1).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.tab_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (!params.at(2).is_null() && OB_FAIL(params.at(2).get_varchar(param.tab_group_))) {
      LOG_WARN("failed to get tblspace", K(ret));
    } else if (!params.at(2).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.tab_group_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (!params.at(3).is_null() && OB_FAIL(params.at(3).get_bool(is_temp_table))) {
      LOG_WARN("failed to get global_temporary", K(ret));
    } else if (param.tab_name_.empty()) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Statistics table must be specified", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Statistics table must be specified");
    } else if (is_temp_table) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("dbms_stats with temp table not support", K(ret));
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "dbms_stats with temp table");
    } else if (param.db_name_.empty()) {
      param.db_name_ = session->get_user_name();
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObDbmsStatsExportImport::create_stat_table(ctx, param))) {// Note:
        LOG_WARN("failed to create table stats", K(ret));
      } else {
        LOG_TRACE("succeed to create table stat", K(param));
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::drop_stat_table
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. stattab          VARCHAR2,
 * @param result
 * @return
 */
int ObDbmsStats::drop_stat_table(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam param;
  param.allocator_ = &ctx.get_allocator();
  ObSQLSessionInfo *session = ctx.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(session));
  } else {
    param.tenant_id_ = session->get_effective_tenant_id();
    if (!params.at(0).is_null() && OB_FAIL(params.at(0).get_varchar(param.db_name_))) {
      LOG_WARN("failed to get db_name", K(ret));
    } else if (!params.at(0).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.db_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (!params.at(1).is_null() && OB_FAIL(params.at(1).get_varchar(param.tab_name_))) {
      LOG_WARN("failed to get tab_name", K(ret));
    } else if (!params.at(1).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.tab_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (param.tab_name_.empty()) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Statistics table must be specified", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Statistics table must be specified");
    } else if (param.db_name_.empty()) {
      param.db_name_ = session->get_user_name();
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(ObDbmsStatsExportImport::drop_stat_table(ctx, param))) {// Note:
        LOG_WARN("failed to drop table stats", K(ret));
      } else {
        LOG_TRACE("succeed to drop table stat", K(param));
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::export_table_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. tabname          VARCHAR2,
 *      2. partname         VARCHAR2 DEFAULT NULL,
 *      3. stattab          VARCHAR2
 *      4. statid           VARCHAR2 DEFAULT NULL,
 *      5. cascade          BOOLEAN DEFAULT TRUE,
 *      6. statown          VARCHAR2 DEFAULT NULL,
 *      7. stat_category    VARCHAR2 DEFAULT DEFAULT_STAT_CATEGORY
 * @param result
 * @return
 */
int ObDbmsStats::export_table_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  SMART_VAR(ObTableStatParam, stat_param) {
    stat_param.allocator_ = &ctx.get_allocator();
    ObTableStatParam stat_table_param;
    stat_table_param.allocator_ = &ctx.get_allocator();
    ObString empty_string;
    const share::schema::ObTableSchema *table_schema = NULL;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(parse_table_part_info(ctx,
                                            params.at(0),
                                            params.at(1),
                                            params.at(2),
                                            stat_param))) {
      LOG_WARN("failed to parse owner", K(ret));
    } else if (OB_FAIL(parse_table_info(ctx,
                                        params.at(6).is_null() ? params.at(0) : params.at(6),
                                        params.at(3),
                                        false,
                                        table_schema,
                                        stat_table_param))) {
      LOG_WARN("failed to parse table info", K(ret));
    } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table schema is null", K(ret), K(table_schema), K(stat_table_param.db_name_),
                                       K(stat_table_param.tab_name_));
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                        to_cstring(stat_table_param.tab_name_));
    } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_varchar(stat_param.stat_id_))) {
      LOG_WARN("failed to get stat id", K(ret));
    } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(stat_param.cascade_))) {
      LOG_WARN("failed to get cascade ", K(ret));
    } else if (!params.at(7).is_null() &&
              OB_FAIL(params.at(7).get_varchar(stat_param.stat_category_))) {
      LOG_WARN("failed to get stat category ", K(ret));
    } else if (!params.at(7).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*stat_table_param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                stat_param.stat_category_))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (OB_FAIL(parse_stat_category(stat_param.stat_category_))) {
      LOG_WARN("failed to parse stat category", K(ret), K(stat_param.stat_category_));
    } else {
      stat_param.stat_own_ = stat_table_param.db_name_;
      stat_param.stat_tab_ = stat_table_param.tab_name_;
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObDbmsStatsExportImport::export_table_stats(ctx, stat_param, empty_string))) {// Note:
      LOG_WARN("failed to export table stats", K(ret));
    } else if (stat_param.cascade_ && stat_param.part_name_.empty() &&
              OB_FAIL(export_table_index_stats(ctx, stat_param))) {
      LOG_WARN("failed to export table index stats", K(ret));
    } else {
      LOG_TRACE("succeed to export table stats", K(stat_param));
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::export_column_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. tabname       VARCHAR2,
 *      2. colname       VARCHAR2,
 *      3. partname      VARCHAR2 DEFAULT NULL,
 *      4. stattab       VARCHAR2,
 *      5. statid        VARCHAR2 DEFAULT NULL,
 *      6. statown       VARCHAR2 DEFAULT NULL
 * @param result
 * @return
 */
int ObDbmsStats::export_column_stats(sql::ObExecContext &ctx,
                                     sql::ParamStore &params,
                                     common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  ObTableStatParam stat_table_param;
  stat_table_param.allocator_ = &ctx.get_allocator();
  const share::schema::ObTableSchema *table_schema = NULL;
  stat_param.cascade_ = true;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(3),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(parse_column_info(ctx, params.at(2), stat_param))) {
    LOG_WARN("failed to parse export column info", K(ret));
  } else if (OB_FAIL(parse_table_info(ctx,
                                      params.at(6).is_null() ? params.at(0) : params.at(6),
                                      params.at(4),
                                      false,
                                      table_schema,
                                      stat_table_param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema),
                                     K(stat_table_param.db_name_), K(stat_table_param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                       to_cstring(stat_table_param.tab_name_));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_varchar((stat_param.stat_id_)))) {
    LOG_WARN("failed to get stat id ", K(ret));
  } else {
    stat_param.stat_own_ = stat_table_param.db_name_;
    stat_param.stat_tab_ = stat_table_param.tab_name_;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObDbmsStatsExportImport::export_column_stats(ctx, stat_param))) {// Note:
    LOG_WARN("failed to export column stats", K(ret));
  } else {
    LOG_TRACE("succeed to export column stats", K(stat_param));
  }
  return ret;
}

/**
 * @brief ObDbmsStat::export_schema_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. stattab          VARCHAR2
 *      2. statid           VARCHAR2 DEFAULT NULL,
 *      3. statown          VARCHAR2 DEFAULT NULL,
 * @param result
 * @return
 */
int ObDbmsStats::export_schema_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  SMART_VAR(ObTableStatParam, global_param) {
    global_param.allocator_ = &ctx.get_allocator();
    ObTableStatParam stat_table_param;
    stat_table_param.allocator_ = &ctx.get_allocator();
    const share::schema::ObTableSchema *table_schema = NULL;
    ObSEArray<uint64_t, 4> table_ids;
    ObString tmp_str;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
      LOG_WARN("failed to get all table ids in database", K(ret));
    } else if (OB_FAIL(parse_table_info(ctx,
                                        params.at(3).is_null() ? params.at(0) : params.at(3),
                                        params.at(1),
                                        false,
                                        table_schema,
                                        stat_table_param))) {
      LOG_WARN("failed to parse table info", K(ret));
    } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table schema is null", K(ret), K(table_schema),
                                      K(stat_table_param.db_name_), K(stat_table_param.tab_name_));
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                        to_cstring(stat_table_param.tab_name_));
    } else if (!params.at(2).is_null() && OB_FAIL(params.at(5).get_varchar((stat_table_param.stat_id_)))) {
      LOG_WARN("failed to get stat id ", K(ret));
    } else {
      ObArenaAllocator tmp_alloc("OptStatExport", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        StatTable stat_table;
        stat_table.database_id_ = global_param.db_id_;
        stat_table.table_id_ = table_ids.at(i);
        SMART_VAR(ObTableStatParam, stat_param) {
          stat_param = global_param;
          stat_param.stat_own_ = stat_table_param.db_name_;
          stat_param.stat_tab_ = stat_table_param.tab_name_;
          stat_param.stat_id_ = stat_table_param.stat_id_;
          stat_param.cascade_ = true;
          stat_param.allocator_ = &tmp_alloc;//use the temp allocator to free memory after export stats.
          if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
            LOG_WARN("failed to parse table part info", K(ret));
          } else if (OB_FAIL(ObDbmsStatsExportImport::export_table_stats(ctx, stat_param, tmp_str))) {// Note:
            LOG_WARN("failed to export table stats", K(ret));
          } else if (OB_FAIL(export_table_index_stats(ctx, stat_param))) {
            LOG_WARN("failed to export table index stats", K(ret));
          } else {
            tmp_alloc.reset();
            LOG_TRACE("succeed to export table stats", K(stat_param));
          }
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::export_index_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. indname          VARCHAR2,
 *      2. partname         VARCHAR2 DEFAULT NULL,
 *      3. stattab          VARCHAR2
 *      4. statid           VARCHAR2 DEFAULT NULL,
 *      5. statown          VARCHAR2 DEFAULT NULL,
 *      6. tabname          VARCHAR2 DEFAULT NULL(for mysql mode only)
 * @param result
 * @return
 */
int ObDbmsStats::export_index_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam index_stat_param;
  index_stat_param.allocator_ = &ctx.get_allocator();
  index_stat_param.is_index_stat_ = true;
  ObTableStatParam stat_table_param;
  stat_table_param.allocator_ = &ctx.get_allocator();
  const share::schema::ObTableSchema *table_schema = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode() && !params.at(6).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name shouldn't be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL,"table name shouldn't be specified in gather index stats");
  } else if (lib::is_mysql_mode() && params.at(6).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name should be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "table name should be specified in gather index stats");
  } else if (OB_FAIL(parse_index_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           params.at(6),
                                           index_stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(parse_table_info(ctx,
                                      params.at(5).is_null() ? params.at(0) : params.at(5),
                                      params.at(3),
                                      false,
                                      table_schema,
                                      stat_table_param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(stat_table_param.db_name_),
                                     K(stat_table_param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                       to_cstring(stat_table_param.tab_name_));
  } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_varchar(index_stat_param.stat_id_))) {
    LOG_WARN("failed to get stat id", K(ret));
  } else {
    index_stat_param.stat_own_ = stat_table_param.db_name_;
    index_stat_param.stat_tab_ = stat_table_param.tab_name_;
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(ObDbmsStatsExportImport::export_table_stats(ctx, index_stat_param,
                                                                 index_stat_param.data_table_name_))) {// Note:
    LOG_WARN("failed to export table stats", K(ret));
  } else {
    LOG_TRACE("succeed to export table stats", K(index_stat_param));
  }
  return ret;
}

int ObDbmsStats::export_table_index_stats(sql::ObExecContext &ctx,
                                          const ObTableStatParam data_param)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  if (OB_FAIL(get_table_index_infos(ctx, data_param.table_id_, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = data_param.db_id_;
      stat_table.table_id_ = simple_index_infos.at(i).table_id_;
      ObTableStatParam index_param;
      index_param.assign_common_property(data_param);
      if (simple_index_infos.at(i).table_id_ == data_param.table_id_) {
        //do nothing, remove primary table
      } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else {
        index_param.is_index_stat_ = true;
        if (OB_FAIL(ObDbmsStatsExportImport::export_table_stats(ctx,
                                                                index_param,
                                                                data_param.tab_name_))) {// Note:
          LOG_WARN("failed to export table stats", K(ret));
        } else {/*do nothing*/}
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::import_table_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. tabname          VARCHAR2,
 *      2. partname         VARCHAR2 DEFAULT NULL,
 *      3. stattab          VARCHAR2
 *      4. statid           VARCHAR2 DEFAULT NULL,
 *      5. cascade          BOOLEAN DEFAULT TRUE,
 *      6. statown          VARCHAR2 DEFAULT NULL,
 *      7. no_invalidate    BOOLEAN DEFAULT FALSE,
 *      8. force            BOOLEAN DEFAULT FALSE,
 *      9. stat_category    VARCHAR2 DEFAULT DEFAULT_STAT_CATEGORY
 * @param result
 * @return
 */
int ObDbmsStats::import_table_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  SMART_VAR(ObTableStatParam, stat_table_param) {
    stat_table_param.allocator_ = &ctx.get_allocator();
    ObTableStatParam stat_param;
    stat_param.allocator_ = &ctx.get_allocator();
    bool cascade_index = false;
    const share::schema::ObTableSchema *table_schema = NULL;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(parse_table_part_info(ctx,
                                            params.at(0),
                                            params.at(1),
                                            params.at(2),
                                            stat_param))) {
      LOG_WARN("failed to parse owner", K(ret));
    } else if (OB_FAIL(parse_table_info(ctx,
                                        params.at(6).is_null() ? params.at(0) : params.at(6),
                                        params.at(3),
                                        false,
                                        table_schema,
                                        stat_table_param))) {
      LOG_WARN("failed to parse table info", K(ret));
    } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table schema is null", K(ret), K(table_schema), K(stat_table_param.db_name_),
                                      K(stat_table_param.tab_name_));
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                        to_cstring(stat_table_param.tab_name_));
    } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_varchar(stat_param.stat_id_))) {
      LOG_WARN("failed to get stat id ", K(ret));
    } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(stat_param.cascade_))) {
      LOG_WARN("failed to get stat cascade ", K(ret));
    } else if (!params.at(7).is_null() && OB_FAIL(params.at(7).get_bool(stat_param.no_invalidate_))) {
      LOG_WARN("failed to get stat no_invalidate ", K(ret));
    } else if (!params.at(8).is_null() && OB_FAIL(params.at(8).get_bool(stat_param.force_))) {
      LOG_WARN("failed to get stat force ", K(ret));
    } else if (!params.at(9).is_null() &&
              OB_FAIL(params.at(9).get_varchar(stat_param.stat_category_))) {
      LOG_WARN("failed to get stat stat_category ", K(ret));
    } else if (!params.at(9).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*stat_param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                stat_param.stat_category_))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (OB_FAIL(parse_stat_category(stat_param.stat_category_))) {
      LOG_WARN("failed to parse stat category", K(ret), K(stat_param.stat_category_));
    } else {
      cascade_index = stat_param.cascade_;
      stat_param.stat_own_ = stat_table_param.db_name_;
      stat_param.stat_tab_ = stat_table_param.tab_name_;
      decide_modified_part(stat_param, true /* cascade_part */);
      if (!stat_param.part_name_.empty()) {
        cascade_index = false;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (stat_param.force_ &&
              OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed fill stat locked", K(ret));
    } else if (!stat_param.force_ &&
              OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
      LOG_WARN("failed check stat locked", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExportImport::import_table_stats(ctx, stat_param))) {// Note:
      LOG_WARN("failed to import table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
      LOG_WARN("failed to update stat cache", K(ret));
    } else if (cascade_index && stat_param.part_name_.empty() &&
              OB_FAIL(import_table_index_stats(ctx, stat_param))) {
      LOG_WARN("failed to import table index stats", K(ret));
    } else {
      LOG_TRACE("succeed to import table stats", K(stat_param));
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::import_column_stats
 * @param ctx
 * @param params
 *      0. ownname       VARCHAR2,
 *      1. tabname       VARCHAR2,
 *      2. colname       VARCHAR2,
 *      3. partname      VARCHAR2 DEFAULT NULL,
 *      4. stattab       VARCHAR2,
 *      5. statid        VARCHAR2 DEFAULT NULL,
 *      6. statown       VARCHAR2 DEFAULT NULL
 *      7. no_invalidate BOOLEAN DEFAULT FALSE,
 *      8. force         BOOLEAN DEFAULT FALSE
 * @param result
 * @return
 */
int ObDbmsStats::import_column_stats(sql::ObExecContext &ctx,
                                     sql::ParamStore &params,
                                     common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  ObTableStatParam stat_table_param;
  stat_param.allocator_ = &ctx.get_allocator();
  stat_table_param.allocator_ = &ctx.get_allocator();
  const share::schema::ObTableSchema *table_schema = NULL;
  stat_param.cascade_ = true;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(3),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(parse_column_info(ctx, params.at(2), stat_param))) {
    LOG_WARN("failed to parse column info", K(ret));
  } else if (OB_FAIL(parse_table_info(ctx,
                                      params.at(6).is_null() ? params.at(0) : params.at(6),
                                      params.at(4),
                                      false,
                                      table_schema,
                                      stat_table_param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema),
                                     K(stat_table_param.db_name_), K(stat_table_param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                       to_cstring(stat_table_param.tab_name_));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_varchar(stat_param.stat_id_))) {
    LOG_WARN("failed to get stat id ", K(ret));
  } else if (!params.at(7).is_null() && OB_FAIL(params.at(7).get_bool(stat_param.no_invalidate_))) {
    LOG_WARN("failed to get no_invalidate ", K(ret));
  } else if (!params.at(8).is_null() && OB_FAIL(params.at(8).get_bool(stat_param.force_))) {
    LOG_WARN("failed to get force ", K(ret));
  } else {
    stat_param.stat_own_ = stat_table_param.db_name_;
    stat_param.stat_tab_ = stat_table_param.tab_name_;
    decide_modified_part(stat_param, true /* cascade_part */);
  }
  if (OB_FAIL(ret)) {
  } else if (!stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExportImport::import_column_stats(ctx, stat_param))) {// Note:
    LOG_WARN("failed to import column stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to import column stats", K(stat_param));
  }
  return ret;
}

/**
 * @brief ObDbmsStat::import_schema_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. stattab          VARCHAR2
 *      2. statid           VARCHAR2 DEFAULT NULL,
 *      3. statown          VARCHAR2 DEFAULT NULL,
 *      4. no_invalidate    BOOLEAN DEFAULT FALSE,
 *      5. force            BOOLEAN DEFAULT FALSE,
 * @param result
 * @return
 */
int ObDbmsStats::import_schema_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  SMART_VAR(ObTableStatParam, global_param) {
    global_param.allocator_ = &ctx.get_allocator();
    ObTableStatParam stat_table_param;
    stat_table_param.allocator_ = &ctx.get_allocator();
    const share::schema::ObTableSchema *table_schema = NULL;
    ObSEArray<uint64_t, 4> table_ids;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
      LOG_WARN("failed to get all table ids in database", K(ret));
    } else if (OB_FAIL(parse_table_info(ctx,
                                        params.at(3).is_null() ? params.at(0) : params.at(3),
                                        params.at(1),
                                        false,
                                        table_schema,
                                        stat_table_param))) {
      LOG_WARN("failed to parse table info", K(ret));
    } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("table schema is null", K(ret), K(table_schema), K(stat_table_param.db_name_),
                                      K(stat_table_param.tab_name_));
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                        to_cstring(stat_table_param.tab_name_));
    } else if (!params.at(2).is_null() && OB_FAIL(params.at(4).get_varchar(stat_table_param.stat_id_))) {
      LOG_WARN("failed to get stat id ", K(ret));
    } else {
      ObArenaAllocator tmp_alloc("OptStatImport", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        StatTable stat_table;
        stat_table.database_id_ = global_param.db_id_;
        stat_table.table_id_ = table_ids.at(i);
        SMART_VAR(ObTableStatParam, stat_param) {
          stat_param = global_param;
          stat_param.stat_own_ = stat_table_param.db_name_;
          stat_param.stat_tab_ = stat_table_param.tab_name_;
          stat_param.stat_id_ = stat_table_param.stat_id_;
          stat_param.cascade_ = true;
          stat_param.global_stat_param_.need_modify_ = true;
          stat_param.part_stat_param_.need_modify_ = true;
          stat_param.subpart_stat_param_.need_modify_ = true;
          stat_param.allocator_ = &tmp_alloc;//use the temp allocator to free memory after stat import
          if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
            LOG_WARN("failed to parse table part info", K(ret));
          } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_bool(stat_param.no_invalidate_))) {
            LOG_WARN("failed to get stat no_invalidate ", K(ret));
          } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(stat_param.force_))) {
            LOG_WARN("failed to get stat force ", K(ret));
          } else if (stat_param.force_ &&
                    OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
            LOG_WARN("failed fill stat locked", K(ret));
          } else if (!stat_param.force_ &&
                    OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
            if (OB_ERR_DBMS_STATS_PL == ret) {
              // all table/partition locked, just skip
              ret = OB_SUCCESS;
              LOG_TRACE("table locked, just skip", K(stat_param));
            } else {
              LOG_WARN("failed to check stat locked", K(ret));
            }
          } else if (OB_FAIL(ObDbmsStatsExportImport::import_table_stats(ctx, stat_param))) {// Note:
            LOG_WARN("failed to import table stats", K(ret));
          } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
            LOG_WARN("failed to update stat cache", K(ret));
          } else if (OB_FAIL(import_table_index_stats(ctx, stat_param))) {
            LOG_WARN("failed to import table index stats", K(ret));
          } else {
            tmp_alloc.reset();
            LOG_TRACE("succeed to import table stats", K(stat_param));
          }
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStat::import_index_stats
 * @param ctx
 * @param params
 *      0. ownname          VARCHAR2,
 *      1. indname          VARCHAR2,
 *      2. partname         VARCHAR2 DEFAULT NULL,
 *      3. stattab          VARCHAR2
 *      4. statid           VARCHAR2 DEFAULT NULL,
 *      5. statown          VARCHAR2 DEFAULT NULL,
 *      6. no_invalidate    BOOLEAN DEFAULT FALSE,
 *      7. force            BOOLEAN DEFAULT FALSE,
 *      8. tabname          VARCHAR2 DEFAULT NULL(for mysql mode only)
 * @param result
 * @return
 */
int ObDbmsStats::import_index_stats(ObExecContext &ctx, ParamStore &params, ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_table_param;
  ObTableStatParam index_stat_param;
  stat_table_param.allocator_ = &ctx.get_allocator();
  index_stat_param.allocator_ = &ctx.get_allocator();
  index_stat_param.is_index_stat_ = true;
  const share::schema::ObTableSchema *table_schema = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode() && !params.at(8).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name shouldn't be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL,"table name shouldn't be specified in gather index stats");
  } else if (lib::is_mysql_mode() && params.at(8).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("table name should be specified in gather index stats", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "table name should be specified in gather index stats");
  } else if (OB_FAIL(parse_index_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           params.at(8),
                                           index_stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(parse_table_info(ctx,
                                      params.at(5).is_null() ? params.at(0) : params.at(5),
                                      params.at(3),
                                      false,
                                      table_schema,
                                      stat_table_param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(stat_table_param.db_name_),
                                     K(stat_table_param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(stat_table_param.db_name_),
                                       to_cstring(stat_table_param.tab_name_));
  } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_varchar(index_stat_param.stat_id_))) {
    LOG_WARN("failed to get stat id ", K(ret));
  } else if (!params.at(6).is_null() && OB_FAIL(params.at(6).get_bool(index_stat_param.no_invalidate_))) {
    LOG_WARN("failed to get stat no_invalidate ", K(ret));
  } else if (!params.at(7).is_null() && OB_FAIL(params.at(7).get_bool(index_stat_param.force_))) {
    LOG_WARN("failed to get stat force ", K(ret));
  } else {
    index_stat_param.stat_own_ = stat_table_param.db_name_;
    index_stat_param.stat_tab_ = stat_table_param.tab_name_;
    decide_modified_part(index_stat_param, true /* cascade_part */);
  }
  if (OB_FAIL(ret)) {
  } else if (index_stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, index_stat_param))) {
    LOG_WARN("failed fill stat locked", K(ret));
  } else if (!index_stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, index_stat_param))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsExportImport::import_table_stats(ctx, index_stat_param))) {// Note:
    LOG_WARN("failed to import table stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_stat_param))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {
    LOG_TRACE("succeed to import index stats", K(index_stat_param));
  }
  return ret;
}

int ObDbmsStats::import_table_index_stats(sql::ObExecContext &ctx,
                                          const ObTableStatParam data_param)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  if (OB_FAIL(get_table_index_infos(ctx, data_param.table_id_, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = data_param.db_id_;
      stat_table.table_id_ = simple_index_infos.at(i).table_id_;
      ObTableStatParam index_param;
      index_param.assign_common_property(data_param);
      if (simple_index_infos.at(i).table_id_ == data_param.table_id_) {
        //do nothing, remove primary table
      } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else {
        index_param.is_index_stat_ = true;
        if (OB_FAIL(ret)) {
        } else if (index_param.force_ &&
                   OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, index_param))) {
          LOG_WARN("failed fill stat locked", K(ret));
        } else if (!index_param.force_ &&
                   OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, index_param))) {
          LOG_WARN("failed check stat locked", K(ret));
        } else if (OB_FAIL(ObDbmsStatsExportImport::import_table_stats(ctx, index_param))) {// Note:
          LOG_WARN("failed to import table stats", K(ret));
        } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), index_param))) {
          LOG_WARN("failed to update stat cache", K(ret));
        } else {
          LOG_TRACE("succeed to import table index stats", K(index_param));
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::lock_table_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. tabname           VARCHAR2,
 *   2. stattype          VARCHAR2 DEFAULT 'ALL'
 * @param result
 * @return
 */
int ObDbmsStats::lock_table_stats(sql::ObExecContext &ctx,
                                  sql::ParamStore &params,
                                  common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  ObObjParam part_name;
  part_name.set_null();
  ObString stat_type_str;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           part_name,
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(params.at(2).get_varchar(stat_type_str))) {
    LOG_WARN("failed to get stattype", K(ret));
  } else if (OB_FAIL(convert_vaild_ident_name(*stat_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              stat_type_str))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(parse_stat_type(stat_type_str, stat_param.stattype_))) {
    LOG_WARN("failed to parse stat type", K(ret), K(stat_type_str));
  } else {
    stat_param.global_stat_param_.need_modify_ = true;
    stat_param.part_stat_param_.need_modify_ = true;
    stat_param.subpart_stat_param_.need_modify_ = true;
    if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, true))) {// Note:
      LOG_WARN("failed to lock table stats", K(ret));
    } else if (OB_FAIL(lock_or_unlock_index_stats(ctx, stat_param, true))) {
      LOG_WARN("failed to lock index stats", K(ret));
    } else {/*do nothing*/}
  }
  return ret;
}

/**
 * @brief ObDbmsStats::lock_partition_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. tabname           VARCHAR2,
 *   2. partname          VARCHAR2
 * @param result
 * @return
 */
int ObDbmsStats::lock_partition_stats(sql::ObExecContext &ctx,
                                      sql::ParamStore &params,
                                      common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  stat_param.stattype_ = StatTypeLocked::PARTITION_ALL_TYPE;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (params.at(2).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("partition not specified", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "partition not specified");
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  //specify subpart name, do nothing, compatible oracle.
  } else if (!stat_param.part_name_.empty() && stat_param.is_subpart_name_) {
    /*do nothing*/
  } else {
    stat_param.global_stat_param_.need_modify_ = false;
    stat_param.part_stat_param_.need_modify_ = true;
    stat_param.subpart_stat_param_.need_modify_ = false;
    if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, true))) {
      LOG_WARN("failed to lock table stats", K(ret));
    } else {/*do nothing */}
  }
  return ret;
}

/**
 * @brief ObDbmsStats::lock_schema_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. stattype          VARCHAR2 DEFAULT 'ALL'
 * @param result
 * @return
 */
int ObDbmsStats::lock_schema_stats(sql::ObExecContext &ctx,
                                   sql::ParamStore &params,
                                   common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString stat_type_str;
  SMART_VAR(ObTableStatParam, global_param) {
    global_param.allocator_ = &ctx.get_allocator();
    ObSEArray<uint64_t, 4> table_ids;
    if (OB_FAIL(check_statistic_table_writeable(ctx))) {
      LOG_WARN("failed to check tenant is restore", K(ret));
    } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
      LOG_WARN("failed to get all table ids in database", K(ret));
    } else if (OB_FAIL(params.at(1).get_varchar(stat_type_str))) {
      LOG_WARN("failed to get stattype", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*global_param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                stat_type_str))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (OB_FAIL(parse_stat_type(stat_type_str, global_param.stattype_))) {
      LOG_WARN("failed to parse stat type", K(ret), K(stat_type_str));
    } else {
      ObArenaAllocator tmp_alloc("OptStatLock", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        StatTable stat_table;
        stat_table.database_id_ = global_param.db_id_;
        stat_table.table_id_ = table_ids.at(i);
        ObTableStatParam stat_param = global_param;
        if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
          LOG_WARN("failed to parse table part info", K(ret));
        } else {
          stat_param.global_stat_param_.need_modify_ = true;
          stat_param.part_stat_param_.need_modify_ = true;
          stat_param.subpart_stat_param_.need_modify_ = true;
          stat_param.allocator_ = &tmp_alloc;//use the temp allocator free memory after stat lock
          if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, true))) {
            LOG_WARN("failed to lock table stats", K(ret));
          } else if (OB_FAIL(lock_or_unlock_index_stats(ctx, stat_param, true))) {
            LOG_WARN("failed to lock index stats", K(ret));
          } else {
            tmp_alloc.reset();
          }
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::lock_or_unlock_index_stats(sql::ObExecContext &ctx,
                                            const ObTableStatParam data_param,
                                            bool is_lock_stats)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  if (OB_FAIL(get_table_index_infos(ctx, data_param.table_id_, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < simple_index_infos.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = data_param.db_id_;
      stat_table.table_id_ = simple_index_infos.at(i).table_id_;
      ObTableStatParam index_param;
      index_param.assign_common_property(data_param);
      if (simple_index_infos.at(i).table_id_ == data_param.table_id_) {
        //do nothing, remove primary table
      } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, index_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else {
        index_param.global_stat_param_.need_modify_ = true;
        index_param.part_stat_param_.need_modify_ = true;
        index_param.subpart_stat_param_.need_modify_ = true;
        index_param.is_index_stat_ = true;
        if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, index_param, is_lock_stats))) {
          LOG_WARN("failed to lock table stats", K(ret));
        } else {/*do nothing */}
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::unlock_table_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. tabname           VARCHAR2,
 *   2. stattype          VARCHAR2 DEFAULT 'ALL'
 * @param result
 * @return
 */
int ObDbmsStats::unlock_table_stats(sql::ObExecContext &ctx,
                                    sql::ParamStore &params,
                                    common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  ObObjParam part_name;
  part_name.set_null();
  ObString stat_type_str;
  stat_param.allocator_ = &ctx.get_allocator();
  stat_param.stattype_ = StatTypeLocked::TABLE_ALL_TYPE;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           part_name,
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (!params.at(2).is_null() && OB_FAIL(params.at(2).get_varchar(stat_type_str))) {
    LOG_WARN("failed to get stattype", K(ret));
  } else if (!params.at(2).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*stat_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              stat_type_str))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(parse_stat_type(stat_type_str, stat_param.stattype_))) {
    LOG_WARN("failed to parse stat type", K(ret), K(stat_type_str));
  } else {
    stat_param.global_stat_param_.need_modify_ = true;
    stat_param.part_stat_param_.need_modify_ = true;
    stat_param.subpart_stat_param_.need_modify_ = true;
    if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, false))) {
      LOG_WARN("failed to lock table stats", K(ret));
    } else if (OB_FAIL(lock_or_unlock_index_stats(ctx, stat_param, false))) {
      LOG_WARN("failed to lock index stats", K(ret));
    } else {/*do nothing*/}
  }
  return ret;
}

/**
 * @brief ObDbmsStats::unlock_partition_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. tabname           VARCHAR2,
 *   2. partname          VARCHAR2
 * @param result
 * @return
 */
int ObDbmsStats::unlock_partition_stats(sql::ObExecContext &ctx,
                                        sql::ParamStore &params,
                                        common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  stat_param.stattype_ = StatTypeLocked::PARTITION_ALL_TYPE;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (params.at(2).is_null()) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("partition not specified", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "partition not specified");
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           params.at(2),
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  //specify subpart name, do nothing, compatible oracle.
  } else if (!stat_param.part_name_.empty() && stat_param.is_subpart_name_) {
    /*do nothing*/
  } else {
    stat_param.global_stat_param_.need_modify_ = false;
    stat_param.part_stat_param_.need_modify_ = true;
    stat_param.subpart_stat_param_.need_modify_ = false;
    if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, false))) {
      LOG_WARN("failed to lock table stats", K(ret));
    } else {/*do nothing */}
  }
  return ret;
}

/**
 * @brief ObDbmsStats::unlock_schema_stats
 * @param ctx
 * @param params
 *   0. ownname           VARCHAR2,
 *   1. stattype          VARCHAR2 DEFAULT 'ALL'
 * @param result
 * @return
 */
int ObDbmsStats::unlock_schema_stats(sql::ObExecContext &ctx,
                                     sql::ParamStore &params,
                                     common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString stat_type_str;
  SMART_VAR(ObTableStatParam, global_param) {
    ObSEArray<uint64_t, 4> table_ids;
    global_param.stattype_ = StatTypeLocked::TABLE_ALL_TYPE;
    global_param.allocator_ = &ctx.get_allocator();
    if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
      LOG_WARN("failed to get all table ids in database", K(ret));
    } else if (!params.at(1).is_null() && OB_FAIL(params.at(1).get_varchar(stat_type_str))) {
      LOG_WARN("failed to get stattype", K(ret));
    } else if (!params.at(1).is_null() &&
               OB_FAIL(convert_vaild_ident_name(*global_param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                stat_type_str))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (OB_FAIL(parse_stat_type(stat_type_str, global_param.stattype_))) {
      LOG_WARN("failed to parse stat type", K(ret), K(stat_type_str));
    } else {
      ObArenaAllocator tmp_alloc("OptStatUnlock", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
      for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
        StatTable stat_table;
        stat_table.database_id_ = global_param.db_id_;
        stat_table.table_id_ = table_ids.at(i);
        ObTableStatParam stat_param = global_param;
        if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
          LOG_WARN("failed to parse table part info", K(ret));
        } else {
          stat_param.global_stat_param_.need_modify_ = true;
          stat_param.part_stat_param_.need_modify_ = true;
          stat_param.subpart_stat_param_.need_modify_ = true;
          stat_param.allocator_ = &tmp_alloc;//use the temp allocator to free memory after stat unlock
          if (OB_FAIL(ObDbmsStatsLockUnlock::set_table_stats_lock(ctx, stat_param, false))) {
            LOG_WARN("failed to lock table stats", K(ret));
          } else if (OB_FAIL(lock_or_unlock_index_stats(ctx, stat_param, false))) {
            LOG_WARN("failed to lock index stats", K(ret));
          } else {
            tmp_alloc.reset();
          }
        }
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::restore_table_stats
 * @param ctx
 * @param params
 *   0. ownname               VARCHAR2,
 *   1. tabname               VARCHAR2,
 *   2. as_of_timestamp       TIMESTAMP,
 *   3. restore_cluster_index BOOLEAN DEFAULT FALSE,
 *   4. force                 BOOLEAN DEFAULT FALSE,
 *   5. no_invalidate         BOOLEAN DEFAULT FALSE
 * @param result
 * @return
 */
int ObDbmsStats::restore_table_stats(sql::ObExecContext &ctx,
                                     sql::ParamStore &params,
                                     common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam stat_param;
  stat_param.allocator_ = &ctx.get_allocator();
  ObObjParam part_name;
  part_name.set_null();
  ObString stat_type_str;
  bool restore_cluster_index = false;
  int64_t specify_time = 0;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx,
                                           params.at(0),
                                           params.at(1),
                                           part_name,
                                           stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (lib::is_oracle_mode()) {
    if (params.at(2).is_null()) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Invalid or inconsistent input values", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid or inconsistent input values");
    } else if (!params.at(2).is_timestamp_tz()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(2)),
                                       K(get_type_name(params.at(2).get_type())));
    } else if (params.at(2).is_timestamp_tz()) {
      specify_time = params.at(2).get_otimestamp_value().time_us_;
    }
  } else if (lib::is_mysql_mode()) {
    if (params.at(2).is_null()) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Invalid or inconsistent input values", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid or inconsistent input values");
    } else if (!params.at(2).is_datetime()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(2)),
                                       K(get_type_name(params.at(2).get_type())));
    } else if (params.at(2).is_datetime()) {
      specify_time = params.at(2).get_datetime();
      if (OB_FAIL(ObTimeConverter::datetime_to_timestamp(specify_time,
                                                         get_timezone_info(ctx.get_my_session()),
                                                         specify_time))) {
        LOG_WARN("failed to datetime to timestamp", K(ret), K(specify_time));
      }
    }
  }
  //check timestamp;
  if (OB_SUCC(ret)) {
    ObObj tmp_timestamp;
    const int64_t current_time = ObTimeUtility::current_time();
    int64_t min_savetime = 0;
    if (OB_FAIL(ObDbmsStatsHistoryManager::get_stats_history_retention_and_availability(ctx, false, tmp_timestamp))) {// Note:
      LOG_WARN("failed to get min save time", K(ret));
    } else if (tmp_timestamp.is_null()) {
      //do nothing
    } else if (OB_FAIL(tmp_timestamp.get_timestamp(min_savetime))) {
      LOG_WARN("failed to get timestamp", K(ret));
    } else if (specify_time < min_savetime) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Unable to restore statistics, statistics history not available", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Unable to restore statistics, statistics history not available");
    } else if (specify_time > current_time) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Invalid or inconsistent input values", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid or inconsistent input values");
    }
  }

  if (OB_FAIL(ret)) {
  } else if (!params.at(3).is_null() && OB_FAIL(params.at(3).get_bool(restore_cluster_index))) {
    LOG_WARN("failed to get restore cluster index", K(ret));
  } else if (!params.at(4).is_null() && OB_FAIL(params.at(4).get_bool(stat_param.force_))) {
    LOG_WARN("failed to get force", K(ret));
  } else if (!params.at(5).is_null() && OB_FAIL(params.at(5).get_bool(stat_param.no_invalidate_))) {
    LOG_WARN("failed to get no_invalidate", K(ret));
  } else if (stat_param.is_temp_table_) {//do nothing
  // oracle don't do this, compatible oracle temporarily
  // } else if (stat_param.force_ &&
  //            OB_FAIL(ObDbmsStatsLockUnlock::fill_stat_locked(ctx, stat_param))) {
  //   LOG_WARN("failed fill stat locked", K(ret));
  } else if (!stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
    LOG_WARN("failed check stat locked", K(ret));
  } else if (OB_FAIL(ObDbmsStatsHistoryManager::restore_table_stats(ctx,
                                                                    stat_param,
                                                                    specify_time))) {// Note:
    LOG_WARN("failed restore table stats", K(ret));
  } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
    LOG_WARN("failed to update stat cache", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::restore_schema_stats
 * @param ctx
 * @param params
 *   0. ownname               VARCHAR2,
 *   1. as_of_timestamp       TIMESTAMP,
 *   2. force                 BOOLEAN DEFAULT FALSE,
 *   3. no_invalidate         BOOLEAN DEFAULT FALSE
 * @param result
 * @return
 */
int ObDbmsStats::restore_schema_stats(sql::ObExecContext &ctx,
                                      sql::ParamStore &params,
                                      common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObTableStatParam global_param;
  global_param.allocator_ = &ctx.get_allocator();
  ObSEArray<uint64_t, 4> table_ids;
  int64_t specify_time = 0;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
    LOG_WARN("failed to get all table ids in database", K(ret));
  } else if (lib::is_oracle_mode()) {
    if (!params.at(1).is_null() && !params.at(1).is_timestamp_tz()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(1)),
                                       K(get_type_name(params.at(1).get_type())));
    } else if (params.at(1).is_timestamp_tz()) {
      specify_time = params.at(1).get_otimestamp_value().time_us_;
    }
  } else if (lib::is_mysql_mode()) {
    if (!params.at(1).is_null() && !params.at(1).is_datetime()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(1)),
                                       K(get_type_name(params.at(1).get_type())));
    } else if (params.at(1).is_datetime()) {
      specify_time = params.at(1).get_datetime();
      if (OB_FAIL(ObTimeConverter::datetime_to_timestamp(specify_time,
                                                         get_timezone_info(ctx.get_my_session()),
                                                         specify_time))) {
        LOG_WARN("failed to datetime to timestamp", K(ret), K(specify_time));
      }
    }
  }
  if (OB_SUCC(ret)) {
    ObArenaAllocator tmp_alloc("OptStatRestore", OB_MALLOC_NORMAL_BLOCK_SIZE, global_param.tenant_id_);
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
      StatTable stat_table;
      stat_table.database_id_ = global_param.db_id_;
      stat_table.table_id_ = table_ids.at(i);
      ObTableStatParam stat_param = global_param;
      stat_param.allocator_ = &tmp_alloc;////use the temp allocator to free memory after stat restore
      if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
        LOG_WARN("failed to parse table part info", K(ret));
      } else if (!params.at(2).is_null() && OB_FAIL(params.at(2).get_bool(stat_param.force_))) {
        LOG_WARN("failed to get force", K(ret));
      } else if (!params.at(3).is_null() && OB_FAIL(params.at(3).get_bool(stat_param.no_invalidate_))) {
        LOG_WARN("failed to get no_invalidate", K(ret));
      } else if (!stat_param.force_ &&
             OB_FAIL(ObDbmsStatsLockUnlock::check_stat_locked(ctx, stat_param))) {
        if (OB_ERR_DBMS_STATS_PL == ret) {
          // all table/partition locked, just skip
          ret = OB_SUCCESS;
          LOG_TRACE("table locked, just skip", K(stat_param));
        } else {
          LOG_WARN("failed to check stat locked", K(ret));
        }
      } else if (OB_FAIL(ObDbmsStatsHistoryManager::restore_table_stats(ctx,
                                                                        stat_param,
                                                                        specify_time))) {// Note:
        LOG_WARN("failed restore table stats", K(ret));
      } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(), stat_param))) {
        LOG_WARN("failed to update stat cache", K(ret));
      } else {
        tmp_alloc.reset();
        LOG_TRACE("Succeed to restore table stats", K(stat_param), K(specify_time));
      }
    }
  }
  return ret;
}

/**
 * @brief ObDbmsStats::purge_stats
 * @param ctx
 * @param params
 *   0. as_of_timestamp       TIMESTAMP
 * @param result
 * @return
 */
int ObDbmsStats::purge_stats(sql::ObExecContext &ctx,
                             sql::ParamStore &params,
                             common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  int64_t specify_time = -1;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (lib::is_oracle_mode()) {
    if (!params.at(0).is_null() && !params.at(0).is_timestamp_tz()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(0)),
                                       K(get_type_name(params.at(0).get_type())));
    } else if (params.at(0).is_timestamp_tz()) {
      specify_time = params.at(0).get_otimestamp_value().time_us_;
    }
  } else if (lib::is_mysql_mode()) {
    if (!params.at(0).is_null() && !params.at(0).is_datetime()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(ret), K(params.at(0)),
                                       K(get_type_name(params.at(0).get_type())));
    } else if (params.at(0).is_datetime()) {
      specify_time = params.at(0).get_datetime();
      if (OB_FAIL(ObTimeConverter::datetime_to_timestamp(specify_time,
                                                         get_timezone_info(ctx.get_my_session()),
                                                         specify_time))) {
        LOG_WARN("failed to datetime to timestamp", K(ret), K(specify_time));
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (ObDbmsStatsHistoryManager::purge_stats(ctx, specify_time)) {// Note:
      LOG_WARN("failed to purge stats", K(ret));
    } else {/*do nothing*/}
  }
  return ret;
}

/**
 * @brief ObDbmsStats::alter_stats_history_retention
 * @param ctx
 * @param params
 *   0. retention       NUMBER
 * @param result
 * @return
 *
 *
 * The retention time in days. The statistics history will be
  retained for at least these many number of days.The valid
  range is [1,365000]. Also you can use the following values for
  special purposes:
  ■ -1: Statistics history is never purged by automatic purge
  ■ 0: Old statistics are never saved. The automatic purge will delete all statistics history
  ■ NULL: Change statistics history retention to default value
 */
int ObDbmsStats::alter_stats_history_retention(sql::ObExecContext &ctx,
                                               sql::ParamStore &params,
                                               common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  number::ObNumber num_retention;
  int64_t new_retention = OPT_DEFAULT_STATS_RETENTION;//default value
  double retention_tmp = 0.0; // bugfix:
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (!params.at(0).is_null() && OB_FAIL(params.at(0).get_number(num_retention))) {
    LOG_WARN("failed to get epc", K(ret));
  } else if (!params.at(0).is_null() &&
             OB_FAIL((ObDbmsStatsUtils::cast_number_to_double(num_retention, retention_tmp)))) {
    LOG_WARN("cast number to double fail", K(ret), K(num_retention));
  } else if (!params.at(0).is_null() &&
             OB_FAIL(num_retention.extract_valid_int64_with_trunc(new_retention))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_retention));
  } else if ((retention_tmp > -1 && retention_tmp < 0) || retention_tmp < -1 || new_retention > 365000) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("Invalid or inconsistent input values", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid or inconsistent input values");
  } else if (OB_FAIL(ObDbmsStatsHistoryManager::alter_stats_history_retention(ctx,
                                                                              new_retention))) {// Note:
    LOG_WARN("failed to alter_stats_history_retention", K(ret), K(new_retention));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::get_stats_history_availability
 * @param ctx
 * @param params
 *  no param
 * @param result
 * @return
 *  RETURN TIMESTAMP WITH TIME ZONE
 */
int ObDbmsStats::get_stats_history_availability(sql::ObExecContext &ctx,
                                                sql::ParamStore &params,
                                                common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(params);
  if (OB_ISNULL(ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(ctx.get_my_session()));
  } else if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(ObDbmsStatsHistoryManager::get_stats_history_retention_and_availability(ctx, false, result))) {// Note:
    LOG_WARN("failed to get stats history availability", K(ret));
  } else if (result.is_null()) {
    //do nothing
  } else if (lib::is_oracle_mode() && !result.is_timestamp_tz()) {
    ObObj dest_obj;
    ObCastCtx cast_ctx(&ctx.get_allocator(), NULL, CM_NONE, ObCharset::get_system_collation());
    cast_ctx.dtc_params_ = ctx.get_my_session()->get_dtc_params();
    if (OB_FAIL(ObObjCaster::to_type(ObTimestampTZType, cast_ctx, result, dest_obj))) {
      LOG_WARN("failed to ObTimestampTZType type", K(ret));
    } else {
      result = dest_obj;
    }
  } else if (lib::is_mysql_mode() && !result.is_datetime()) {
    ObObj dest_obj;
    ObCastCtx cast_ctx(&ctx.get_allocator(), NULL, CM_NONE, ObCharset::get_system_collation());
    cast_ctx.dtc_params_ = ctx.get_my_session()->get_dtc_params();
    if (OB_FAIL(ObObjCaster::to_type(ObDateTimeType, cast_ctx, result, dest_obj))) {
      LOG_WARN("failed to ObTimestampType type", K(ret));
    } else {
      result = dest_obj;
    }
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::get_stats_history_retention
 * @param ctx
 * @param params
 *  no param
 * @param result
 * @return
 *  RETURN NUMBER
 */
int ObDbmsStats::get_stats_history_retention(sql::ObExecContext &ctx,
                                             sql::ParamStore &params,
                                             common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(params);
  ObObj retention;
  number::ObNumber num_retention;
  int64_t retention_val = 0;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(ObDbmsStatsHistoryManager::get_stats_history_retention_and_availability(ctx, true, retention))) {// Note:
    LOG_WARN("failed to get stats history retention", K(ret));
  } else if (OB_FAIL(retention.get_number(num_retention))) {
    LOG_WARN("failed to get int", K(ret), K(retention));
  } else if (OB_FAIL(num_retention.extract_valid_int64_with_trunc(retention_val))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_retention));
  } else if (retention_val == MAX_HISTORY_RETENTION) {
    retention_val = -1;
    if (OB_FAIL(num_retention.from(retention_val, ctx.get_allocator()))) {
      LOG_WARN("convert int to number failed", K(ret));
    } else {
      result.set_number(num_retention);
    }
  } else {
    result = retention;
  }
  return ret;
}

/**
 * @brief ObDbmsStats::reset_global_pref_defaults
 * @param ctx
 * @param params
 *  no param
 * @param result
 * @return
 */
int ObDbmsStats::reset_global_pref_defaults(sql::ObExecContext &ctx,
                                            sql::ParamStore &params,
                                            common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(params);
  UNUSED(result);
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(ObDbmsStatsPreferences::reset_global_pref_defaults(ctx))) {
    LOG_WARN("failed to reset global pref defaults");
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::get_prefs
 * @param ctx
 * @param params
 *  pname           VARCHAR2,
 *  ownname         VARCHAR2 DEFAULT NULL,
 *  tabname         VARCHAR2 DEFAULT NULL
 * @param result
 * @return
 *  return varchar2
 */
int ObDbmsStats::get_prefs(sql::ObExecContext &ctx,
                           sql::ParamStore &params,
                           common::ObObj &result)
{
  int ret = OB_SUCCESS;
  ObString opt_name;
  ObString dummy_name;
  ObObjParam dummy_param;
  dummy_param.set_null();
  ObTableStatParam param;
  param.allocator_ = &ctx.get_allocator();
  ObStatPrefs *stat_pref = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (!params.at(0).is_null() && OB_FAIL(params.at(0).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret));
  } else if (!params.at(0).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!params.at(2).is_null() &&
             OB_FAIL(parse_table_part_info(ctx, params.at(1), params.at(2), dummy_param, param))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(2)));
  } else if (OB_FAIL(get_new_stat_pref(ctx, *param.allocator_, opt_name, dummy_name, true, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  } else if (FALSE_IT(stat_pref->set_is_global_prefs(true))) {
  } else if (OB_FAIL(ObDbmsStatsPreferences::get_prefs(ctx, param, opt_name, result))) {
    LOG_WARN("failed to get prefs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::set_global_prefs
 * @param ctx
 * @param params
 *  pname         VARCHAR2,
 *  pvalue        VARCHAR2,
 * @param result
 * @return
 */
int ObDbmsStats::set_global_prefs(sql::ObExecContext &ctx,
                                  sql::ParamStore &params,
                                  common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString opt_name;
  ObString opt_value;
  ObSEArray<uint64_t, 4> table_ids;
  ObStatPrefs *stat_pref = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (!params.at(0).is_null() && OB_FAIL(params.at(0).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(0)));
  } else if (!params.at(0).is_null() &&
             OB_FAIL(convert_vaild_ident_name(ctx.get_allocator(),
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!params.at(1).is_null() && OB_FAIL(params.at(1).get_string(opt_value))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(1)));
  } else if (!params.at(1).is_null() &&
             OB_FAIL(convert_vaild_ident_name(ctx.get_allocator(),
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_value))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_new_stat_pref(ctx, ctx.get_allocator(), opt_name, opt_value, true, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  } else if (FALSE_IT(stat_pref->set_is_global_prefs(true))) {
  } else if (OB_FAIL(stat_pref->check_pref_value_validity())) {
    LOG_WARN("failed to check pref value validity");
  } else if (OB_FAIL(stat_pref->dump_pref_name_and_value(opt_name, opt_value))) {
    LOG_WARN("failed to dump pref name and value");
  } else if (OB_FAIL(ObDbmsStatsPreferences::set_prefs(ctx, table_ids, opt_name, opt_value))) {
    LOG_WARN("failed to set prefs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::set_schema_prefs
 * @param ctx
 * @param params
 *  ownname        VARCHAR2,
 *  pname          VARCHAR2,
 *  pvalue         VARCHAR2
 * @param result
 * @return
 */
int ObDbmsStats::set_schema_prefs(sql::ObExecContext &ctx,
                                  sql::ParamStore &params,
                                  common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString opt_name;
  ObString opt_value;
  ObSEArray<uint64_t, 4> table_ids;
  ObTableStatParam global_param;
  global_param.allocator_ = &ctx.get_allocator();
  ObStatPrefs *stat_pref = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), global_param, table_ids))) {
    LOG_WARN("failed to get all table ids in database", K(ret));
  } else if (!params.at(1).is_null() && OB_FAIL(params.at(1).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(1)));
  } else if (!params.at(1).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*global_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!params.at(2).is_null() && OB_FAIL(params.at(2).get_string(opt_value))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(2)));
  } else if (!params.at(2).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*global_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_value))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_new_stat_pref(ctx, *global_param.allocator_, opt_name, opt_value, false, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  } else if (FALSE_IT(stat_pref->set_is_global_prefs(true))) {
  } else if (OB_FAIL(stat_pref->check_pref_value_validity())) {
    LOG_WARN("failed to check pref value validity");
  } else if (OB_FAIL(stat_pref->dump_pref_name_and_value(opt_name, opt_value))) {
    LOG_WARN("failed to dump pref name and value");
  } else if (OB_FAIL(ObDbmsStatsPreferences::set_prefs(ctx, table_ids, opt_name, opt_value))) {
    LOG_WARN("failed to set prefs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::set_table_prefs
 * @param ctx
 * @param params
 *  ownname        VARCHAR2,
 *  tabname        VARCHAR2,
 *  pname          VARCHAR2,
 *  pvalue         VARCHAR2
 * @param result
 * @return
 */
int ObDbmsStats::set_table_prefs(sql::ObExecContext &ctx,
                                 sql::ParamStore &params,
                                 common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString opt_name;
  ObString opt_value;
  ObObjParam dummy_param;
  dummy_param.set_null();
  ObTableStatParam param;
  param.allocator_ = &ctx.get_allocator();
  ObSEArray<uint64_t, 4> table_ids;
  ObStatPrefs *stat_pref = NULL;
  bool use_size_auto = false;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx, params.at(0), params.at(1), dummy_param, param))) {
    LOG_WARN("failed to get string", K(ret));
  } else if (OB_FAIL(table_ids.push_back(param.table_id_))) {
    LOG_WARN("failed to push back", K(ret));
  } else if (!params.at(2).is_null() && OB_FAIL(params.at(2).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(2)));
  } else if (!params.at(2).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!params.at(3).is_null() && OB_FAIL(params.at(3).get_string(opt_value))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(3)));
  } else if (!params.at(3).is_null() &&
             OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_value))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_new_stat_pref(ctx, *param.allocator_, opt_name, opt_value, false, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  } else if (OB_FAIL(stat_pref->check_pref_value_validity())) {
    LOG_WARN("failed to check pref value validity");
  } else if (OB_FAIL(stat_pref->dump_pref_name_and_value(opt_name, opt_value))) {
    LOG_WARN("failed to dump pref name and value");
  } else if (0 == opt_name.case_compare("METHOD_OPT") &&
             OB_FAIL(parse_method_opt(ctx, param.allocator_, param.column_params_, opt_value, use_size_auto))) {
    LOG_WARN("failed to parse method opt", K(ret));
  } else if (OB_FAIL(ObDbmsStatsPreferences::set_prefs(ctx, table_ids, opt_name, opt_value))) {
    LOG_WARN("failed to set prefs", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::delete_schema_prefs
 * @param ctx
 * @param params
 *  ownname        VARCHAR2,
 *  pname          VARCHAR2
 * @param result
 * @return
 */
int ObDbmsStats::delete_schema_prefs(sql::ObExecContext &ctx,
                                     sql::ParamStore &params,
                                     common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString opt_name;
  ObString dummy_name;
  ObSEArray<uint64_t, 4> table_ids;
  ObTableStatParam dummy_param;
  dummy_param.allocator_ = &ctx.get_allocator();
  ObStatPrefs *stat_pref = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(get_all_table_ids_in_database(ctx, params.at(0), dummy_param, table_ids))) {
    LOG_WARN("failed to get all table ids in database", K(ret));
  } else if (params.at(1).is_null()) {
    // if pname is null, do not check stat prefs.
  } else if (OB_FAIL(params.at(1).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(1)));
  } else if (OB_FAIL(convert_vaild_ident_name(*dummy_param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_new_stat_pref(ctx, *dummy_param.allocator_, opt_name, dummy_name, false, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  }
  if (OB_SUCC(ret) && OB_FAIL(ObDbmsStatsPreferences::delete_user_prefs(ctx, table_ids, opt_name))) {
    LOG_WARN("failed to delete user prefs", K(ret));
  }
  return ret;
}

/**
 * @brief ObDbmsStats::delete_table_prefs
 * @param ctx
 * @param params
 *  ownname        VARCHAR2,
 *  tabname        VARCHAR2,
 *  pname          VARCHAR2,
 * @param result
 * @return
 */
int ObDbmsStats::delete_table_prefs(sql::ObExecContext &ctx,
                                    sql::ParamStore &params,
                                    common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  ObString opt_name;
  ObString dummy_name;
  ObObjParam dummy_param;
  dummy_param.set_null();
  ObTableStatParam param;
  param.allocator_ = &ctx.get_allocator();
  ObSEArray<uint64_t, 4> table_ids;
  ObStatPrefs *stat_pref = NULL;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx, params.at(0), params.at(1), dummy_param, param))) {
    LOG_WARN("failed to get string", K(ret));
  } else if (OB_FAIL(table_ids.push_back(param.table_id_))) {
    LOG_WARN("failed to push back", K(ret));
  } else if (params.at(2).is_null()) {
    //if pname is null, skip check prefs.
  } else if (OB_FAIL(params.at(2).get_string(opt_name))) {
    LOG_WARN("failed to get string", K(ret), K(params.at(2)));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              opt_name))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_new_stat_pref(ctx, *param.allocator_, opt_name, dummy_name, false, stat_pref))) {
    LOG_WARN("failed to get new stat pref", K(ret));
  } else if (OB_ISNULL(stat_pref)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(stat_pref));
  }
  if (OB_SUCC(ret) && OB_FAIL(ObDbmsStatsPreferences::delete_user_prefs(ctx, table_ids, opt_name))) {
    LOG_WARN("failed to delete user prefs", K(ret));
  }
  return ret;
}

int ObDbmsStats::update_stat_cache(const uint64_t rpc_tenant_id,
                                   const ObTableStatParam &param,
                                   ObOptStatRunningMonitor *running_monitor/*default null*/)
{
  int ret = OB_SUCCESS;
  obrpc::ObUpdateStatCacheArg stat_arg;
  stat_arg.tenant_id_ = param.tenant_id_;
  stat_arg.table_id_ = param.table_id_;
  stat_arg.no_invalidate_ = param.no_invalidate_;
  for (int64_t i = 0; OB_SUCC(ret) && i < param.column_params_.count(); ++i) {
    if (OB_FAIL(stat_arg.column_ids_.push_back(param.column_params_.at(i).column_id_))) {
      LOG_WARN("failed to push back column id", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < param.part_infos_.count(); ++i) {
    if (OB_FAIL(stat_arg.partition_ids_.push_back(param.part_infos_.at(i).part_id_))) {
      LOG_WARN("failed to push back partition id", K(ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < param.subpart_infos_.count(); ++i) {
    if (OB_FAIL(stat_arg.partition_ids_.push_back(param.subpart_infos_.at(i).part_id_))) {
      LOG_WARN("failed to push back partition id", K(ret));
    }
  }
  if (OB_SUCC(ret) && param.global_stat_param_.need_modify_) {
    int64_t part_id = param.global_part_id_;
    if (OB_FAIL(stat_arg.partition_ids_.push_back(part_id))) {
      LOG_WARN("failed to push back partition ids", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    LOG_TRACE("update stat cache", K(stat_arg));
    bool evict_plan_failed = false;
    int64_t timeout = -1;
    ObSEArray<ObServerLocality, 4> all_server_arr;
    bool has_read_only_zone = false; // UNUSED;
    if (OB_ISNULL(GCTX.srv_rpc_proxy_) || OB_ISNULL(GCTX.locality_manager_)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("rpc_proxy or session is null", K(ret), K(GCTX.srv_rpc_proxy_), K(GCTX.locality_manager_));
    } else if (OB_FAIL(GCTX.locality_manager_->get_server_locality_array(all_server_arr,
                                                                         has_read_only_zone))) {
      LOG_WARN("fail to get server locality", K(ret));
    } else {
      ObSEArray<ObServerLocality, 4> failed_server_arr;
      for (int64_t i = 0; OB_SUCC(ret) && i < all_server_arr.count(); i++) {
        if (!all_server_arr.at(i).is_active()
            || ObServerStatus::OB_SERVER_ACTIVE != all_server_arr.at(i).get_server_status()
            || 0 == all_server_arr.at(i).get_start_service_time()
            || 0 != all_server_arr.at(i).get_server_stop_time()) {
        //server may not serving
        } else if (0 >= (timeout = THIS_WORKER.get_timeout_remain())) {
          ret = OB_TIMEOUT;
          LOG_WARN("query timeout is reached", K(ret), K(timeout));
        } else if (OB_FAIL(GCTX.srv_rpc_proxy_->to(all_server_arr.at(i).get_addr())
                                                  .timeout(timeout)
                                                  .by(rpc_tenant_id)
                                                  .update_local_stat_cache(stat_arg))) {
          LOG_WARN("failed to update local stat cache caused by unknow error",
                                           K(ret), K(all_server_arr.at(i).get_addr()), K(stat_arg));
          if (OB_FAIL(failed_server_arr.push_back(all_server_arr.at(i)))) {
            LOG_WARN("failed to push back", K(ret));
          }
        }
      }
      LOG_TRACE("update stat cache", K(param), K(stat_arg), K(failed_server_arr), K(all_server_arr));
      if (OB_SUCC(ret) && !failed_server_arr.empty() && running_monitor != NULL) {
        ObSqlString tmp_str;
        char *buf = NULL;
        if (failed_server_arr.count() * (common::MAX_IP_ADDR_LENGTH + 1) <= common::MAX_VALUE_LENGTH) {
          for (int64_t i = 0; OB_SUCC(ret) && i < failed_server_arr.count(); ++i) {
            char svr_buf[common::MAX_IP_ADDR_LENGTH] = {0};
            failed_server_arr.at(i).get_addr().to_string(svr_buf, common::MAX_IP_ADDR_LENGTH);
            if (OB_FAIL(tmp_str.append_fmt("%s%s", svr_buf, i == 0 ? "" : ","))) {
              LOG_WARN("failed to append fmt", K(ret));
            }
          }
        } else if (OB_FAIL(tmp_str.append_fmt("more than %ld servers refresh stat cache failed",
                                              failed_server_arr.count()))) {
          LOG_WARN("failed to append fmt", K(ret));
        }
        if (OB_FAIL(ret)) {
          //do nothing
        } else if (OB_ISNULL(buf = static_cast<char*>(running_monitor->allocator_.alloc(tmp_str.length())))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("memory is not enough", K(ret), K(tmp_str));
        } else {
          MEMCPY(buf, tmp_str.ptr(), tmp_str.length());
          running_monitor->opt_stat_gather_stat_.set_stat_refresh_failed_list(buf, tmp_str.length());
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::parse_table_part_info(ObExecContext &ctx,
                                       const ObObjParam &owner,
                                       const ObObjParam &tab_name,
                                       const ObObjParam &part_name,
                                       ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *table_schema = NULL;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(schema_guard), K(param.allocator_));
  } else if (OB_FAIL(parse_table_info(ctx, owner, tab_name, false, table_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(param.db_name_), K(param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(param.db_name_), to_cstring(param.tab_name_));
  } else if (OB_FAIL(get_table_part_infos(table_schema,
                                          param.part_infos_,
                                          param.subpart_infos_,
                                          param.part_ids_,
                                          param.subpart_ids_))) {
    LOG_WARN("failed to get table part infos", K(ret));
  } else if (OB_FAIL(param.all_part_infos_.assign(param.part_infos_)) ||
             OB_FAIL(param.all_subpart_infos_.assign(param.subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (!part_name.is_null() && OB_FAIL(parse_partition_name(ctx, table_schema, part_name, param))) {
    LOG_WARN("failed to parse partition name", K(ret));
  } else {
    param.table_id_ = table_schema->get_table_id();
    param.ref_table_type_ = table_schema->get_table_type();
    param.part_level_ = table_schema->get_part_level();
    param.total_part_cnt_ = table_schema->get_all_part_num();
    // we can't get part/subpart type anyway, because default value of part_func_type is
    // PARTITION_FUNC_TYPE_HASH even table is not partitioned.
    if (share::schema::ObPartitionLevel::PARTITION_LEVEL_ONE == param.part_level_) {
      param.part_stat_param_.part_type_ = table_schema->get_part_option().get_part_func_type();
    } else if (share::schema::ObPartitionLevel::PARTITION_LEVEL_TWO == param.part_level_) {
      param.part_stat_param_.part_type_ = table_schema->get_part_option().get_part_func_type();
      param.subpart_stat_param_.part_type_ = table_schema->get_sub_part_option().get_part_func_type();
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(init_column_stat_params(*param.allocator_,
                                        *schema_guard,
                                        *table_schema,
                                        param.column_params_))) {
      LOG_WARN("failed to init column stat params", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::parse_table_part_info(ObExecContext &ctx,
                                       const StatTable stat_table,
                                       ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *table_schema = NULL;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(schema_guard), K(param));
  } else if (OB_FAIL(parse_table_info(ctx, stat_table, table_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(get_table_part_infos(table_schema,
                                          param.part_infos_,
                                          param.subpart_infos_,
                                          param.part_ids_,
                                          param.subpart_ids_))) {
    LOG_WARN("failed to get table part infos", K(ret));
  } else if (OB_FAIL(param.all_part_infos_.assign(param.part_infos_)) ||
             OB_FAIL(param.all_subpart_infos_.assign(param.subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(init_column_stat_params(*param.allocator_,
                                             *schema_guard,
                                             *table_schema,
                                             param.column_params_))) {
    LOG_WARN("failed to init column stat params", K(ret));
  } else {
    param.table_id_ = table_schema->get_table_id();
    param.ref_table_type_ = table_schema->get_table_type();
    param.part_level_ = table_schema->get_part_level();
    param.total_part_cnt_ = table_schema->get_all_part_num();
  }
  return ret;
}

int ObDbmsStats::parse_index_part_info(ObExecContext &ctx,
                                       const ObObjParam &owner,
                                       const ObObjParam &index_name,
                                       const ObObjParam &part_name,
                                       const ObObjParam &table_name,
                                       ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *index_schema = NULL;
  const share::schema::ObTableSchema *table_schema = NULL;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(schema_guard), K(param));
  } else if (table_name.is_null() &&
             OB_FAIL(parse_table_info(ctx, owner, index_name, true,
                                      index_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (!table_name.is_null() &&
             OB_FAIL(parse_index_table_info(ctx, owner, table_name, index_name,
                                            index_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(index_schema)) {
    ret = OB_ERR_INDEX_UNKNOWN;
    LOG_WARN("index schema is null", K(ret), K(index_schema), K(param.db_name_),
                                     K(param.tab_name_));
    LOG_USER_ERROR(OB_ERR_INDEX_UNKNOWN);
  } else if (OB_FAIL(schema_guard->get_table_schema(index_schema->get_tenant_id(),
                                                    index_schema->get_data_table_id(),
                                                    table_schema))) {
    LOG_WARN("failed to get table schema", K(ret));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(index_schema), K(param.db_name_),
                                     K(param.tab_name_));
  } else if (OB_FAIL(set_param_global_part_id(ctx, param, true, table_schema->get_table_id(),
                                              table_schema->get_part_level()))) {
    LOG_WARN("fail to set global part id for index data table", K(ret));
  } else if (OB_FAIL(ob_write_string(*param.allocator_,
                                     table_schema->get_table_name_str(),
                                     param.data_table_name_))) {
    LOG_WARN("failed to write string", K(ret));
  } else if (OB_FAIL(get_table_part_infos(index_schema,
                                          param.part_infos_,
                                          param.subpart_infos_,
                                          param.part_ids_,
                                          param.subpart_ids_))) {
    LOG_WARN("failed to get table part infos", K(ret));
  } else if (OB_FAIL(param.all_part_infos_.assign(param.part_infos_)) ||
             OB_FAIL(param.all_subpart_infos_.assign(param.subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (!part_name.is_null()) {
    if (OB_FAIL(parse_partition_name(ctx, index_schema, part_name, param))) {
      LOG_WARN("failed to parse partition name", K(ret));
    } else {/*do nothing*/}
  }
  if (OB_SUCC(ret)) {
    param.table_id_ = index_schema->get_table_id();
    param.ref_table_type_ = index_schema->get_table_type();
    param.part_level_ = index_schema->get_part_level();
    param.total_part_cnt_ = index_schema->get_all_part_num();
    param.is_global_index_ = index_schema->is_global_index_table();
    param.data_table_id_ = table_schema->get_table_id();
    if (OB_FAIL(init_column_stat_params(*param.allocator_,
                                        *schema_guard,
                                        *index_schema,
                                        param.column_params_))) {
      LOG_WARN("failed to init column stat params", K(ret));
    } else {
      LOG_TRACE("Succed to parse index part info", K(param));
    }
  }
  return ret;
}

// we be used in  ObLogPlan::allocate_optimizer_stats_gathering_as_top.
// We extract it as a independent function to avoid redudant code.
bool ObDbmsStats::check_column_validity(const share::schema::ObTableSchema &tab_schema,
                                       const share::schema::ObColumnSchemaV2 &col_schema)
{
  bool is_valid = false;
  if (col_schema.is_hidden() &&
      (!tab_schema.is_index_table() ||
        col_schema.get_column_id() < OB_END_RESERVED_COLUMN_ID_NUM ||
        col_schema.is_shadow_column())) {
    //pass
  } else {
    is_valid = true;
  }
  return is_valid;
}

/// init column stats with conf 'for all column size auto'
int ObDbmsStats::init_column_stat_params(ObIAllocator &allocator,
                                         share::schema::ObSchemaGetterGuard &schema_guard,
                                         const ObTableSchema &table_schema,
                                         ObIArray<ObColumnStatParam> &column_params)
{
  int ret = OB_SUCCESS;
  column_params.reset();
  for (int64_t i = 0; OB_SUCC(ret) && i < table_schema.get_column_count(); ++i) {
    const share::schema::ObColumnSchemaV2 *col = table_schema.get_column_schema_by_idx(i);
    ObColumnStatParam col_param;
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column is null", K(ret), K(col));
    //here add extra column id condition, because func index in oracle mode, the column will mark is
    //hidden, that's will cause the fewer columns.
    } else if (!check_column_validity(table_schema, *col)){
      continue;
    } else if (OB_FAIL(ob_write_string(allocator,
                                       col->get_column_name_str(),
                                       col_param.column_name_))) {
      LOG_WARN("failed to write column name", K(ret));
    } else {
      col_param.column_id_ = col->get_column_id();
      col_param.cs_type_   = col->get_collation_type();
      col_param.gather_flag_ = 0;
      col_param.set_size_manual();
      col_param.bucket_num_ = -1;
      col_param.column_attribute_ = 0;
      if (lib::is_oracle_mode() && col->get_meta_type().is_varbinary_or_binary()) {
        //oracle don't have this type. but agent table will have this type, such as "SYS"."ALL_VIRTUAL_COLUMN_REAL_AGENT"
      } else {
        //check basic column type
        if (ObColumnStatParam::is_valid_opt_col_type(col->get_meta_type().get_type())) {
          col_param.set_valid_opt_col();
        }
        //check need avglen
        if (ObColumnStatParam::is_valid_avglen_type(col->get_meta_type().get_type())) {
          col_param.set_need_avg_len();
        }
      }
      if (col->is_rowkey_column() && !table_schema.is_heap_table()) {
        col_param.set_is_index_column();
        if (1 == table_schema.get_rowkey_column_num()) {
          col_param.set_is_unique_column();
        }
      }
      // TODO : for all hidden column means all function based index column
      //        now in OB, these columns is hidden which make column unselectable.
      //        These column should be invisible intead of hidden. Right now ignore hidden column
      // col_param.set_is_hidden_column();
      if (col->is_hidden()) {//now func index in oracle mode, the column will mark is hidden.
        col_param.set_is_hidden_column();
      }
      if (!col->is_nullable()) {
        col_param.set_is_not_null_column();
      }
      if (OB_SUCC(ret) && OB_FAIL(column_params.push_back(col_param))) {
        LOG_WARN("failed to push back column param", K(ret));
      }
    }
  }
  uint64_t tids[OB_MAX_INDEX_PER_TABLE];
  int64_t index_count = OB_MAX_INDEX_PER_TABLE;
  const ObTableSchema *index_schema = NULL;
  const uint64_t tenant_id = table_schema.get_tenant_id();
  if (OB_FAIL(ret)) {//do nothing
  } else if (OB_FAIL(schema_guard.get_can_read_index_array(tenant_id,
                                                           table_schema.get_table_id(),
                                                           tids,
                                                           index_count,
                                                           false, /*with_mv*/
                                                           true, /*with_global_index*/
                                                           false /*domain index*/))) {
    LOG_WARN("failed to get can read index", K(table_schema.get_table_id()), K(ret));
  } else if (index_count > OB_MAX_INDEX_PER_TABLE) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("Invalid index count", K(table_schema.get_table_id()), K(index_count), K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < index_count; ++i) {
      if (OB_FAIL(schema_guard.get_table_schema(tenant_id, tids[i], index_schema))) {
        LOG_WARN("failed to get index schema", K(ret), K(tenant_id), K(tids[i]));
      } else if (OB_ISNULL(index_schema)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < index_schema->get_column_count(); ++j) {
          const share::schema::ObColumnSchemaV2 *col = index_schema->get_column_schema_by_idx(j);
          if (OB_ISNULL(col)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("column is null", K(ret), K(col));
          } else if (col->is_hidden()) {
            continue;
          } else {
            int64_t k = 0;
            bool find = false;
            for (; !find && k < column_params.count(); ++k) {
              if (column_params.at(k).column_id_ == col->get_column_id()) {
                find = true;
              }
            }
            if (find && col->is_index_column()) {
              column_params.at(k - 1).set_is_index_column();
              if (index_schema->is_unique_index() && 1 == index_schema->get_index_column_num()) {
                column_params.at(k - 1).set_is_unique_column();
              }
            }
          }
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::set_default_column_params(ObIArray<ObColumnStatParam> &column_params)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_params.count(); ++i) {
    ObColumnStatParam &param = column_params.at(i);
    if (param.is_valid_opt_col()) {
      param.set_need_basic_stat();
      param.set_size_auto();
      param.column_usage_flag_ = 0;
      param.bucket_num_ = 1;
    }
  }
  return ret;
}

int ObDbmsStats::parse_set_table_info(ObExecContext &ctx,
                                      const ObObjParam &owner,
                                      const ObObjParam &tab_name,
                                      const ObObjParam &part_name,
                                      ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  const share::schema::ObTableSchema *table_schema = NULL;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(schema_guard), K(param));
  } else if (OB_FAIL(parse_table_info(ctx, owner, tab_name, false, table_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(param.db_name_), K(param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(param.db_name_), to_cstring(param.tab_name_));
  } else if (OB_FAIL(parse_set_partition_name(ctx, table_schema, part_name, param))) {
    LOG_WARN("failed to parser part info", K(ret));
  } else if (OB_FAIL(init_column_stat_params(*param.allocator_,
                                             *schema_guard,
                                             *table_schema,
                                             param.column_params_))) {
    LOG_WARN("failed to init column stat params", K(ret));
  } else {
    param.table_id_ = table_schema->get_table_id();
    param.ref_table_type_ = table_schema->get_table_type();
    param.part_level_ = table_schema->get_part_level();
    decide_modified_part(param, false /* cascade_part */);
  }
  return ret;
}

int ObDbmsStats::parse_set_column_stats(ObExecContext &ctx,
                                        const ObObjParam &owner,
                                        const ObObjParam &tab_name,
                                        const ObObjParam &colname,
                                        const ObObjParam &part_name,
                                        ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *table_schema = NULL;
  const share::schema::ObColumnSchemaV2 *col = NULL;
  ObColumnStatParam col_param;
  ObString column_name;
  if (OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(param));
  } else if (OB_FAIL(parse_table_info(ctx, owner, tab_name, false, table_schema, param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema) || OB_UNLIKELY(table_schema->is_view_table())) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(param.db_name_), K(param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(param.db_name_), to_cstring(param.tab_name_));
  } else if (OB_FAIL(colname.get_string(column_name))) {
    LOG_WARN("failed to get column name", K(ret));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              column_name,
                                              lib::is_oracle_mode()))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_UNLIKELY(column_name.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(colname), K(ret));
  } else {
    bool find_it = false;
    for (int64_t i = 0; OB_SUCC(ret) && !find_it && i < table_schema->get_column_count(); ++i) {
      const share::schema::ObColumnSchemaV2 *tmp_col = table_schema->get_column_schema_by_idx(i);
      if (OB_ISNULL(tmp_col)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(tmp_col));
      } else if ((lib::is_oracle_mode() &&
                  ObCharset::case_sensitive_equal(column_name, tmp_col->get_column_name_str())) ||
                 (!lib::is_oracle_mode() &&
                  ObCharset::case_insensitive_equal(column_name, tmp_col->get_column_name_str()))) {
        if (OB_FAIL(ob_write_string(*param.allocator_,
                                    tmp_col->get_column_name_str(),
                                    col_param.column_name_))) {
          LOG_WARN("failed to write column name", K(ret));
        } else {
          find_it = true;
          col = tmp_col;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (!find_it || OB_ISNULL(col)) {
        ret = OB_WRONG_COLUMN_NAME;
        LOG_WARN("column schema is null", K(ret), K(col), K(param.tab_name_),K(column_name));
        LOG_USER_ERROR(OB_WRONG_COLUMN_NAME, static_cast<int32_t>(column_name.length()), column_name.ptr());
      } else {
        col_param.column_id_ = col->get_column_id();
        col_param.cs_type_   = col->get_collation_type();
        col_param.gather_flag_ = 0;
        col_param.bucket_num_ = -1;
        if (col->is_index_column()) {
          col_param.set_is_index_column();
        }
        // TODO : for all hidden column means all function based index column
        //        now in OB, these columns is hidden which make column unselectable.
        //        These column should be invisible intead of hidden. Right now ignore hidden column
        // col_param.is_hidden_col_ = col->is_generated_column();
        if (OB_FAIL(param.column_params_.push_back(col_param))) {
          LOG_WARN("failed to push back column param", K(ret));
        } else if (OB_FAIL(parse_set_partition_name(ctx, table_schema, part_name, param))) {
          LOG_WARN("failed to parser part info", K(ret));
        } else {
          param.table_id_ = table_schema->get_table_id();
          param.ref_table_type_ = table_schema->get_table_type();
          param.part_level_ = table_schema->get_part_level();
          decide_modified_part(param, false /* cascade_part */);
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::parse_set_partition_name(ObExecContext &ctx,
                                          const share::schema::ObTableSchema *&table_schema,
                                          const ObObjParam &part_name,
                                          ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  ObSEArray<PartInfo, 1> part_infos;
  ObSEArray<PartInfo, 32> subpart_infos;
  if (OB_ISNULL(table_schema) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(table_schema), K(ret), K(param.allocator_));
  } else if (part_name.is_null()) {
    /*do nothing*/
  } else if (OB_FAIL(part_name.get_string(param.part_name_))) {
    LOG_WARN("failed to get part name", K(ret));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              param.part_name_,
                                              lib::is_oracle_mode()))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!table_schema->is_partitioned_table()) {
    ret = OB_ERR_NOT_PARTITIONED;
    LOG_WARN("the target table is not partitioned", K(ret));
  } else if (OB_FAIL(get_table_part_infos(table_schema,
                                          param.part_infos_,
                                          param.subpart_infos_,
                                          param.part_ids_,
                                          param.subpart_ids_))) {
    LOG_WARN("failed to get table part infos", K(ret));
  } else if (OB_FAIL(param.all_part_infos_.assign(param.part_infos_)) ||
             OB_FAIL(param.all_subpart_infos_.assign(param.subpart_infos_))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (OB_FAIL(find_selected_part_infos(param.part_name_,
                                              param.part_infos_,
                                              param.subpart_infos_,
                                              lib::is_oracle_mode(),
                                              part_infos,
                                              subpart_infos,
                                              param.is_subpart_name_))) {
    LOG_WARN("failed to find selected partition infos");
  } else if (OB_FAIL(param.part_infos_.assign(part_infos))) {
    LOG_WARN("failed to assign part infos", K(ret));
  } else if (OB_FAIL(param.subpart_infos_.assign(subpart_infos))) {
    LOG_WARN("failed to assign new subpart infos", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObDbmsStats::parse_partition_name(ObExecContext &ctx,
                                      const share::schema::ObTableSchema *&table_schema,
                                      const ObObjParam &part_name,
                                      ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  ObSEArray<PartInfo, 1> part_infos;
  ObSEArray<PartInfo, 32> subpart_infos;
  if (OB_ISNULL(table_schema) || OB_ISNULL(ctx.get_my_session()) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(table_schema), K(ctx.get_my_session()),
                                    K(param.allocator_), K(ret));
  } else if (OB_FAIL(part_name.get_string(param.part_name_))) {
    LOG_WARN("failed to get part name", K(ret));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              param.part_name_,
                                              lib::is_oracle_mode()))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (!table_schema->is_partitioned_table()) {
    ret = OB_ERR_NOT_PARTITIONED;
    LOG_WARN("the target table is not partitioned", K(ret));
  } else if (OB_FAIL(find_selected_part_infos(param.part_name_,
                                              param.part_infos_,
                                              param.subpart_infos_,
                                              lib::is_oracle_mode(),
                                              part_infos,
                                              subpart_infos,
                                              param.is_subpart_name_))) {
    LOG_WARN("failed to find selected partition infos");
  } else if (OB_FAIL(param.part_infos_.assign(part_infos))) {
    LOG_WARN("failed to assign part infos", K(ret));
  } else if (OB_FAIL(param.subpart_infos_.assign(subpart_infos))) {
    LOG_WARN("failed to assign new subpart infos", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObDbmsStats::parse_table_info(ObExecContext &ctx,
                                  const ObObjParam &owner,
                                  const ObObjParam &tab_name,
                                  const bool is_index,
                                  const share::schema::ObTableSchema *&table_schema,
                                  ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  ObSQLSessionInfo *session = ctx.get_my_session();
  table_schema = NULL;
  bool is_valid = true;
  if (OB_ISNULL(session) || OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(session), K(schema_guard), K(param.allocator_));
  } else {
    param.tenant_id_ = session->get_effective_tenant_id();
    if (owner.is_null()) {
      param.db_name_ = session->get_database_name();
    } else if (OB_FAIL(owner.get_string(param.db_name_))) {
      LOG_WARN("failed to get db name", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.db_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (tab_name.is_null()) {
      is_valid = false;
    } else if (OB_FAIL(tab_name.get_string(param.tab_name_))) {
      LOG_WARN("failed to get table name", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                session->get_dtc_params(),
                                                param.tab_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    }
  }
  // parse owner/database info
  if (OB_SUCC(ret) && is_valid) {
    if (OB_FAIL(schema_guard->get_database_id(param.tenant_id_,
                                              param.db_name_,
                                              param.db_id_))) {
      LOG_WARN("failed to get database id", K(ret));
    } else if (OB_UNLIKELY(OB_INVALID_ID == param.db_id_)) {
      is_valid = false;
    }
  }
  if (OB_SUCC(ret) && is_valid) {
    if (!is_index) {
      if (OB_FAIL(schema_guard->get_table_schema(param.tenant_id_,
                                                 param.db_id_,
                                                 param.tab_name_,
                                                 is_index,
                                                 table_schema))) {
        LOG_WARN("failed to get table schema", K(ret), K(param.db_name_), K(param.tab_name_));
      } else {/*do nothing*/}
    } else {
      if (OB_FAIL(schema_guard->get_idx_schema_by_origin_idx_name(param.tenant_id_, param.db_id_,
                                                                  param.tab_name_, table_schema))) {
        LOG_WARN("failed to get idx schema by origin idx name", K(ret));
      } else {/*do nothing*/}
    }
  }
  if (OB_SUCC(ret) && table_schema != NULL && !table_schema->is_view_table()) {
    param.table_id_ = table_schema->get_table_id();
    param.ref_table_type_ = table_schema->get_table_type();
    param.part_level_ = table_schema->get_part_level();
    if (OB_FAIL(set_param_global_part_id(ctx, param))) {
      LOG_WARN("failed to set param globa part id", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::parse_table_info(ObExecContext &ctx,
                                  const StatTable &stat_table,
                                  const share::schema::ObTableSchema *&table_schema,
                                  ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  const ObDatabaseSchema * database_schema = NULL;
  ObSQLSessionInfo *session = ctx.get_my_session();
  table_schema = NULL;
  bool is_valid = true;
  ObString index_name;
  if (OB_ISNULL(session) || OB_ISNULL(schema_guard) || OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(session), K(schema_guard), K(param));
  } else if (OB_FAIL(schema_guard->get_database_schema(session->get_effective_tenant_id(),
             stat_table.database_id_, database_schema))) {
    LOG_WARN("failed to get database schema", K(ret));
  } else if (OB_FAIL(schema_guard->get_table_schema(session->get_effective_tenant_id(),
             stat_table.table_id_, table_schema))) {
    LOG_WARN("failed to get table schema", K(ret));
  } else if (OB_ISNULL(database_schema) || OB_ISNULL(table_schema)) {
    // table may be droped during auto table statistic gathering, caller should ignore this err code
    ret = OB_TABLE_NOT_EXIST;
  } else if (OB_FAIL(ob_write_string(*param.allocator_,
                                     database_schema->get_database_name_str(),
                                     param.db_name_))) {
    LOG_WARN("failed to write string", K(ret));
  } else if (table_schema->is_index_table() && OB_FAIL(table_schema->get_index_name(index_name))) {
    LOG_WARN("fail to get index name", K(ret));
  } else if (OB_FAIL(ob_write_string(*param.allocator_,
                                     table_schema->is_index_table() ? index_name :
                                                                 table_schema->get_table_name_str(),
                                     param.tab_name_))) {
    LOG_WARN("failed to write string", K(ret));
  } else {
    param.tenant_id_ = session->get_effective_tenant_id();
    param.is_temp_table_ = table_schema->is_tmp_table();
  }
  if (OB_SUCC(ret) && table_schema != NULL && !table_schema->is_view_table()) {
    param.table_id_ = table_schema->get_table_id();
    param.ref_table_type_ = table_schema->get_table_type();
    param.part_level_ = table_schema->get_part_level();
    if (OB_FAIL(set_param_global_part_id(ctx, param))) {
      LOG_WARN("failed to set param globa part id", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::parse_index_table_info(ObExecContext &ctx,
                                        const ObObjParam &owner,
                                        const ObObjParam &tab_name,
                                        const ObObjParam &idx_name,
                                        const share::schema::ObTableSchema *&index_schema,
                                        ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  const share::schema::ObTableSchema *table_schema = NULL;
  index_schema = NULL;
  ObTableStatParam data_table_param;
  data_table_param.allocator_ = param.allocator_;
  ObString index_name;
  if (OB_FAIL(parse_table_info(ctx, owner, tab_name, false, table_schema, data_table_param))) {
    LOG_WARN("failed to parse table info", K(ret));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("table schema is null", K(ret), K(table_schema), K(data_table_param.db_name_),
                                     K(data_table_param.tab_name_));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(data_table_param.db_name_),
                                       to_cstring(data_table_param.tab_name_));
  } else if (OB_FAIL(idx_name.get_string(index_name))) {
    LOG_WARN("failed to get string", K(ret), K(idx_name));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              index_name,
                                              lib::is_oracle_mode()))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else if (OB_FAIL(get_index_schema(ctx,
                                      *param.allocator_,
                                      table_schema->get_table_id(),
                                      lib::is_oracle_mode(),
                                      index_name,
                                      index_schema))) {
    LOG_WARN("failed to get index schema", K(ret), K(index_name));
  } else if (OB_ISNULL(index_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("index schema is null", K(ret), K(index_schema), K(data_table_param.db_name_),
                                    K(index_name));
    LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(data_table_param.db_name_),
                                      to_cstring(index_name));
  } else {
    param.tab_name_ = index_name;
    param.db_name_ = data_table_param.db_name_;
    param.tenant_id_ = data_table_param.tenant_id_;
    param.db_id_ = data_table_param.db_id_;
    param.table_id_ = index_schema->get_table_id();
    param.ref_table_type_ = index_schema->get_table_type();
    param.part_level_ = index_schema->get_part_level();
    if (OB_FAIL(set_param_global_part_id(ctx, param))) {
      LOG_WARN("failed to set param globa part id", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::parse_gather_stat_options(ObExecContext &ctx,
                                           const ObObjParam &est_percent,
                                           const ObObjParam &block_sample,
                                           const ObObjParam &method_opt,
                                           const ObObjParam &degree,
                                           const ObObjParam &granularity,
                                           const ObObjParam &cascade,
                                           const ObObjParam &no_invalidate,
                                           const ObObjParam &force,
                                           ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  int64_t stat_options = StatOptionFlags::OPT_APPROXIMATE_NDV | StatOptionFlags::OPT_ESTIMATE_BLOCK;
  number::ObNumber num_est_percent;
  number::ObNumber num_degree;
  double percent = 0.0;
  if (OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(param.allocator_));
  } else if (est_percent.is_null()) {
    //if specify estimate percent null meanings 100% percent sample
    //https://community.oracle.com/tech/developers/discussion/2205871/null-for-estimate-percent-of-dbms-stats?spm=a2o8d.corp_prod_issue_detail_v2.0.0.316db27cDq1yD6
    param.sample_info_.set_percent(100.0);
  } else if (OB_FAIL(est_percent.get_number(num_est_percent))) {
    LOG_WARN("failed to get number", K(ret));
  } else if (OB_FAIL(ObDbmsStatsUtils::cast_number_to_double(num_est_percent, percent))) {
    LOG_WARN("failed to cast number to double" , K(ret));
  } else if (percent == 0.0) {//use default sample size
    stat_options |= StatOptionFlags::OPT_ESTIMATE_PERCENT;
  } else if (OB_UNLIKELY(percent < 0.000001 || percent > 100.0)) {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("Illegal sample percent: must be in the range[0.000001,100]", K(ret));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Illegal sample percent: must be in the range[0.000001,100]");
  } else {
    param.sample_info_.set_percent(percent);
  }
  if (OB_SUCC(ret)) {
    bool is_block_sample = false;
    if (block_sample.is_null()) {
      stat_options |= StatOptionFlags::OPT_BLOCK_SAMPLE;
    } else if (OB_FAIL(block_sample.get_bool(is_block_sample))) {
      LOG_WARN("failed to get block sample", K(ret));
    } else {
      param.sample_info_.set_is_block_sample(is_block_sample);
    }
  }
  if (OB_SUCC(ret)) {
    if (method_opt.is_null()) {
      // do nothing
    } else if (OB_FAIL(method_opt.get_varchar(param.method_opt_))) {
      LOG_WARN("failed to get method opt", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                param.method_opt_))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (0 == param.method_opt_.case_compare("Z")) {
      stat_options |= StatOptionFlags::OPT_METHOD_OPT;
    }
  }
  if (OB_SUCC(ret)) {
    if (degree.is_null()) {
      stat_options |= StatOptionFlags::OPT_DEGREE;
    } else if (OB_FAIL(degree.get_number(num_degree))) {
      LOG_WARN("failed to get degree", K(ret));
    } else if (OB_FAIL(num_degree.extract_valid_int64_with_trunc(param.degree_))) {
      LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_degree));
    }
  }

  if (OB_SUCC(ret)) {
    if (granularity.is_null()) {
      ret = OB_ERR_DBMS_STATS_PL;
      LOG_WARN("Illegal granularity : must be AUTO | ALL | GLOBAL | PARTITION | SUBPARTITION" \
               "| GLOBAL AND PARTITION | APPROX_GLOBAL AND PARTITION", K(ret));
      LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Illegal granularity : must be AUTO | ALL | GLOBAL |" \
                " PARTITION | SUBPARTITION | GLOBAL AND PARTITION | APPROX_GLOBAL AND PARTITION");
    } else if (OB_FAIL(granularity.get_varchar(param.granularity_))) {
      LOG_WARN("failed to get granularity", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                                ctx.get_my_session()->get_dtc_params(),
                                                param.granularity_))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    } else if (0 == param.granularity_.case_compare("Z")) {
      stat_options |= StatOptionFlags::OPT_GRANULARITY;
    }
  }

  if (OB_SUCC(ret)) {
    if (cascade.is_null()) {
      stat_options |= StatOptionFlags::OPT_CASCADE;
    } else if (OB_FAIL(cascade.get_bool(param.cascade_))) {
      LOG_WARN("failed to get cascade value", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    if (no_invalidate.is_null()) {
      stat_options |= StatOptionFlags::OPT_NO_INVALIDATE;
    } else if (OB_FAIL(no_invalidate.get_bool(param.no_invalidate_))) {
      LOG_WARN("failed to get noinvalidate value", K(ret), K(no_invalidate));
    }
  }

  if (OB_SUCC(ret)) {
    if (force.is_null()) {
      stat_options |= StatOptionFlags::OPT_FORCE;
    } else if (OB_FAIL(force.get_bool(param.force_))) {
      LOG_WARN("failed to get force", K(ret));
    }
  }

  if (OB_SUCC(ret)) {
    if (stat_options > 0 && OB_FAIL(get_default_stat_options(ctx, stat_options, param))) {
      LOG_WARN("failed to get default stat options", K(ret));
    } else if (OB_FAIL(parse_granularity_and_method_opt(ctx, param))) {
      LOG_WARN("failed to parse granularity and method opt", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::use_default_gather_stat_options(ObExecContext &ctx,
                                                 const StatTable &stat_table,
                                                 ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  int64_t stat_options = StatOptionFlags::OPT_STAT_OPTION_ALL;
  const static char *incremental_granularity = "APPROX_GLOBAL AND PARTITION";
  if (stat_table.incremental_stat_) {
    stat_options &= ~StatOptionFlags::OPT_GRANULARITY;
    param.granularity_.assign_ptr(incremental_granularity, strlen(incremental_granularity));
  }
  if (OB_FAIL(get_default_stat_options(ctx, stat_options, param))) {
    LOG_WARN("failed to get default stat options", K(ret));
  } else if (OB_FAIL(parse_granularity_and_method_opt(ctx, param))) {
    LOG_WARN("failed to parse granularity and method opt", K(ret));
  }
  return ret;
}

int ObDbmsStats::get_default_stat_options(ObExecContext &ctx,
                                          const int64_t stat_options,
                                          ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObStatPrefs*, 4> stat_prefs;
  if (OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(param.allocator_));
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_ESTIMATE_PERCENT) {
    ObEstimatePercentPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_BLOCK_SAMPLE) {
    param.sample_info_.set_is_block_sample(false);
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_METHOD_OPT) {
    ObMethodOptPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_DEGREE) {
    ObDegreePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_GRANULARITY) {
    ObGranularityPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_CASCADE) {
    ObCascadePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_STATTAB) {
    param.stat_tab_.reset();
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_STATID) {
    param.stat_id_.reset();
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_STATOWN) {
    param.stat_own_.reset();
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_NO_INVALIDATE) {
    ObNoInvalidatePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_STATTYPE) {
    // not implement
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_FORCE) {
    param.force_ = false;
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_APPROXIMATE_NDV) {
    ObApproximateNdvPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret) && stat_options & StatOptionFlags::OPT_ESTIMATE_BLOCK) {
    ObEstimateBlockPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(*param.allocator_, ctx.get_my_session(), ObString(), tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else if (OB_FAIL(stat_prefs.push_back(tmp_pref))) {
      LOG_WARN("failed to push back", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(ObDbmsStatsPreferences::get_sys_default_stat_options(ctx, stat_prefs, param))) {
      LOG_WARN("failed to get sys default stat options", K(ret));
    } else {
      LOG_TRACE("Succeed get default stat options", K(param));
    }
  }
  return ret;
}

int ObDbmsStats::parse_granularity_and_method_opt(ObExecContext &ctx,
                                                  ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  //virtual table(not include real agent table) doesn't gather histogram.
  bool is_vt = is_virtual_table(param.table_id_);
  bool use_size_auto = false;
  if (0 == param.method_opt_.case_compare("Z") && !is_vt) {
    if (OB_FAIL(set_default_column_params(param.column_params_))) {
      LOG_WARN("failed to set default column params", K(ret));
    } else {
      use_size_auto = true;
    }
  } else {
    // method_opt => null, do not gather histogram, gather basic column stat
    const char *method_opt_str = "FOR ALL COLUMNS SIZE 1";
    if (param.method_opt_.empty() || is_vt) {
      param.method_opt_.assign_ptr(method_opt_str, strlen(method_opt_str));
    }
    if (OB_FAIL(ObDbmsStats::parse_method_opt(ctx, param.allocator_,
                                              param.column_params_,
                                              param.method_opt_,
                                              use_size_auto))) {
      LOG_WARN("failed to parse method opt", K(ret));
    }
  }
  if (OB_SUCC(ret)) {
    ObGranularityType granu_type = ObGranularityType::GRANULARITY_INVALID;
    if (OB_FAIL(ObDbmsStatsUtils::parse_granularity(param.granularity_, granu_type))) {
      LOG_WARN("failed to parse granularity");
    } else if (OB_FAIL(resovle_granularity(granu_type, use_size_auto, param)))  {
      LOG_WARN("failed to resovle granularity", K(granu_type));
    } else if (OB_FAIL(process_not_size_manual_column(ctx, param))) {
      LOG_WARN("failed to process not size manual column", K(ret));
    }
  }
  return ret;
}

int ObDbmsStats::parse_set_table_stat_options(ObExecContext &ctx,
                                              const ObObjParam &stattab,
                                              const ObObjParam &statid,
                                              const ObObjParam &numrows,
                                              const ObObjParam &numblks,
                                              const ObObjParam &avgrlen,
                                              const ObObjParam &flags,
                                              const ObObjParam &statown,
                                              const ObObjParam &no_invalidate,
                                              const ObObjParam &cachedblk,
                                              const ObObjParam &cachehit,
                                              const ObObjParam &force,
                                              const ObObjParam &nummacroblks,
                                              const ObObjParam &nummicroblks,
                                              ObSetTableStatParam &param)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  number::ObNumber num_numrows;
  number::ObNumber num_numblks;
  number::ObNumber num_avgrlen;
  number::ObNumber num_flags;
  number::ObNumber num_cachedblk;
  number::ObNumber num_cachehit;
  number::ObNumber num_nummacroblks;
  number::ObNumber num_nummicroblks;
  if (!stattab.is_null() && OB_FAIL(stattab.get_varchar(param.table_param_.stat_tab_))) {
    LOG_WARN("failed to get stattab", K(ret));
  } else if (!statid.is_null() && OB_FAIL(statid.get_varchar(param.table_param_.stat_id_))) {
    LOG_WARN("failed to get statid", K(ret));
  } else if (!numrows.is_null() && OB_FAIL(numrows.get_number(num_numrows))) {
    LOG_WARN("failed to get numrows", K(ret));
  } else if (!numblks.is_null() && OB_FAIL(numblks.get_number(num_numblks))) {
    LOG_WARN("failed to get numblks", K(ret));
  } else if (!avgrlen.is_null() && OB_FAIL(avgrlen.get_number(num_avgrlen))) {
    LOG_WARN("failed to get avgrlen", K(ret));
  } else if (!flags.is_null() && OB_FAIL(flags.get_number(num_flags))) {
    LOG_WARN("failed to get flags", K(ret));
  } else if (!statown.is_null() && OB_FAIL(statown.get_varchar(param.table_param_.stat_own_))) {
    LOG_WARN("failed to get statown", K(ret));
  } else if (!no_invalidate.is_null() && OB_FAIL(no_invalidate.get_bool(param.table_param_.no_invalidate_))) {
    LOG_WARN("failed to get no_invalidate", K(ret));
  } else if (!cachedblk.is_null() && OB_FAIL(cachedblk.get_number(num_cachedblk))) {
    LOG_WARN("failed to get cachedblk", K(ret));
  } else if (!cachehit.is_null() && OB_FAIL(cachehit.get_number(num_cachehit))) {
    LOG_WARN("failed to get ncachehit", K(ret));
  } else if (!force.is_null() && OB_FAIL(force.get_bool(param.table_param_.force_))) {
    LOG_WARN("failed to get force", K(ret));
  } else if (!nummacroblks.is_null() && OB_FAIL(nummacroblks.get_number(num_nummacroblks))) {
    LOG_WARN("failed to get ncachehit", K(ret));
  } else if (!nummicroblks.is_null() && OB_FAIL(nummicroblks.get_number(num_nummicroblks))) {
    LOG_WARN("failed to get ncachehit", K(ret));
  } else if (OB_FAIL(num_numrows.extract_valid_int64_with_trunc(param.numrows_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_numrows));
  } else if (OB_FAIL(num_numblks.extract_valid_int64_with_trunc(param.numblks_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_numblks));
  } else if (OB_FAIL(num_avgrlen.extract_valid_int64_with_trunc(param.avgrlen_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_avgrlen));
  } else if (OB_FAIL(num_flags.extract_valid_int64_with_trunc(param.flags_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_flags));
  } else if (OB_FAIL(num_cachedblk.extract_valid_int64_with_trunc(param.cachedblk_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_cachedblk));
  } else if (OB_FAIL(num_cachehit.extract_valid_int64_with_trunc(param.cachehit_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_cachehit));
  } else if (OB_FAIL(num_nummacroblks.extract_valid_int64_with_trunc(param.nummacroblks_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_nummacroblks));
  } else if (OB_FAIL(num_nummacroblks.extract_valid_int64_with_trunc(param.nummicroblks_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(nummicroblks));
  } else {/*do nothing*/}
  return ret;
}

int ObDbmsStats::parse_set_column_stats_options(ObExecContext &ctx,
                                                const ObObjParam &stattab,
                                                const ObObjParam &statid,
                                                const ObObjParam &distcnt,
                                                const ObObjParam &density,
                                                const ObObjParam &nullcnt,
                                                const ObObjParam &avgclen,
                                                const ObObjParam &flags,
                                                const ObObjParam &statown,
                                                const ObObjParam &no_invalidate,
                                                const ObObjParam &force,
                                                ObSetColumnStatParam &param)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  number::ObNumber num_distcnt;
  number::ObNumber num_density;
  number::ObNumber num_nullcnt;
  number::ObNumber num_avgclen;
  number::ObNumber num_flags;
  if (!stattab.is_null() && OB_FAIL(stattab.get_varchar(param.table_param_.stat_tab_))) {
    LOG_WARN("failed to get stattab", K(ret));
  } else if (!statid.is_null() && OB_FAIL(statid.get_varchar(param.table_param_.stat_id_))) {
    LOG_WARN("failed to get statid", K(ret));
  } else if (!distcnt.is_null() && OB_FAIL(distcnt.get_number(num_distcnt))) {
    LOG_WARN("failed to get distcnt", K(ret));
  } else if (!density.is_null() && OB_FAIL(density.get_number(num_density))) {
    LOG_WARN("failed to get density", K(ret));
  } else if (!nullcnt.is_null() && OB_FAIL(nullcnt.get_number(num_nullcnt))) {
    LOG_WARN("failed to get nullcnt", K(ret));
  } else if (!avgclen.is_null() && OB_FAIL(avgclen.get_number(num_avgclen))) {
    LOG_WARN("failed to get avgclen", K(ret));
  } else if (!flags.is_null() && OB_FAIL(flags.get_number(num_flags))) {
    LOG_WARN("failed to get flags", K(ret));
  } else if (!statown.is_null() && OB_FAIL(statown.get_varchar(param.table_param_.stat_own_))) {
    LOG_WARN("failed to get statown", K(ret));
  } else if (!no_invalidate.is_null() && OB_FAIL(no_invalidate.get_bool(param.table_param_.no_invalidate_))) {
    LOG_WARN("failed to get no_invalidate", K(ret));
  } else if (!force.is_null() && OB_FAIL(force.get_bool(param.table_param_.force_))) {
    LOG_WARN("failed to get force", K(ret));
  } else if (OB_FAIL(num_distcnt.extract_valid_int64_with_trunc(param.distcnt_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_distcnt));
  } else if (OB_FAIL(ObDbmsStatsUtils::cast_number_to_double(num_density, param.density_))) {
    LOG_WARN("failed to cast number to double" , K(ret), K(num_density));
  } else if (OB_FAIL(num_nullcnt.extract_valid_int64_with_trunc(param.nullcnt_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_nullcnt));
  } else if (OB_FAIL(num_avgclen.extract_valid_int64_with_trunc(param.avgclen_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_avgclen));
  } else if (OB_FAIL(num_flags.extract_valid_int64_with_trunc(param.flags_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_flags));
  } else {/*do nothing*/}
  return ret;
}

/**
 * @brief ObDbmsStats::parse_method_opt
 * @param method_opt
 * syntax:
 *  FOR ALL [INDEXED | HIDDEN] COLUMNS [size_clause]
 *  FOR COLUMNS [size clause] column [size_clause] [,column [size_clause]...]
      size_clause is defined as size_clause := SIZE {integer | REPEAT | AUTO | SKEWONLY}
      column is defined as column := column_name | extension name | extension
     - integer : Number of histogram buckets. Must be in the range [1,254].
     - REPEAT : Collects histograms only on the columns that already have histograms
     - AUTO : Oracle determines the columns to collect histograms based on data distribution and the workload of the columns
     - SKEWONLY : Oracle determines the columns to collect histograms based on the data distribution of the columns
     - column_name : name of a column
     - extension : can be either a column group in the format of (column_name, column_name [, ...]) or an expressionThe default is FOR ALL COLUMNS SIZE AUTO.
 * @return
 */
int ObDbmsStats::parse_method_opt(sql::ObExecContext &ctx,
                                  ObIAllocator *allocator,
                                  ObIArray<ObColumnStatParam> &column_params,
                                  const ObString &method_opt,
                                  bool &use_size_auto)
{
  int ret = OB_SUCCESS;
  use_size_auto = false;
  if (OB_ISNULL(allocator) || OB_ISNULL(ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(allocator), K(ctx.get_my_session()));
  } else {
    ObParser parser(*allocator,
                    ctx.get_my_session()->get_sql_mode(),
                    ctx.get_my_session()->get_local_collation_connection());
    ParseMode parse_mode = DYNAMIC_SQL_MODE;
    ParseResult parse_result;
    const ParseNode *for_stmt = NULL;
    if (OB_FAIL(parser.parse(method_opt, parse_result, parse_mode))) {
      LOG_WARN("failed to parse result", K(ret), K(method_opt));
    } else if (OB_ISNULL(parse_result.result_tree_) ||
              OB_ISNULL(for_stmt = parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("for stmt is invalid", K(ret), K(for_stmt));
    } else if (OB_UNLIKELY(for_stmt->type_ != T_METHOD_OPT_LIST) ||
              OB_UNLIKELY(for_stmt->num_child_ < 1)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("parse node is invalid", K(ret), K(for_stmt->type_), K(T_METHOD_OPT_LIST),
                                        K(for_stmt->num_child_));
    }
    ObSEArray<ObString, 4> all_for_col;
    for (int64_t i = 0; OB_SUCC(ret) && i < for_stmt->num_child_; ++i) {
      ParseNode *child_node = for_stmt->children_[i];
      if (OB_ISNULL(child_node)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(child_node));
      } else if (T_FOR_ALL == child_node->type_) {
        if (OB_FAIL(parser_for_all_clause(child_node, column_params, use_size_auto))) {
          LOG_WARN("failed to parser for all clause", K(ret));
        } else {/*do nothing*/}
      } else if (T_FOR_COLUMNS == child_node->type_) {
        if (OB_FAIL(parser_for_columns_clause(child_node, column_params, all_for_col))) {
          LOG_WARN("failed to parser for all clause", K(ret));
        } else {/*do nothing*/}
      } else {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected node type", K(ret), K(child_node->type_));
      }
    }
  }
  return ret;
}

int ObDbmsStats::parser_for_all_clause(const ParseNode *for_all_node,
                                       ObIArray<ObColumnStatParam> &column_params,
                                       bool &use_size_auto)
{
  int ret = OB_SUCCESS;
  use_size_auto = false;
  if (OB_ISNULL(for_all_node) || OB_UNLIKELY(for_all_node->type_ != T_FOR_ALL)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid argument", K(for_all_node), K(ret));
  } else {
    MethodOptColConf for_all_conf;
    MethodOptSizeConf size_conf;
    ParseNode *first  = NULL;
    if (OB_UNLIKELY(2 != for_all_node->num_child_) ||
        OB_ISNULL(first  = for_all_node->children_[0]) ||
        OB_UNLIKELY(first->type_ != T_INT) ||
        OB_UNLIKELY(first->value_ < 0 || first->value_ > 2)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid for all node", K(ret), K(first), K(for_all_node->num_child_));
    } else if (first->value_ == 0) {
      for_all_conf = FOR_ALL;
    } else if (first->value_ == 1) {
      for_all_conf = FOR_INDEXED;
    } else if (first->value_ == 2) {
      for_all_conf = FOR_HIDDEN;
    }
    if (OB_SUCC(ret) && NULL != for_all_node->children_[1]) {
      if (OB_FAIL(parse_size_clause(for_all_node->children_[1], size_conf))) {
        LOG_WARN("failed to parse size clause", K(ret));
      } else {
        use_size_auto = size_conf.is_auto();
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_params.count(); ++i) {
      ObColumnStatParam &col_param = column_params.at(i);
      if (!is_match_column_option(col_param, for_all_conf)) {
        // do nothing
      } else if (!col_param.is_valid_opt_col()) {
        // do nothing
      } else if (OB_FAIL(compute_bucket_num(column_params.at(i), size_conf))) {
        LOG_WARN("failed to compute histogram size", K(ret));
      }
    }
  }
  return ret;
}

int ObDbmsStats::parser_for_columns_clause(const ParseNode *for_col_node,
                                           ObIArray<ObColumnStatParam> &column_params,
                                           ObIArray<ObString> &record_cols)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(for_col_node) ||
      OB_UNLIKELY(for_col_node->type_ != T_FOR_COLUMNS || for_col_node->num_child_ < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid argument", K(for_col_node), K(ret));
  } else {
    MethodOptSizeConf default_size_conf;
    for (int64_t i = 0; OB_SUCC(ret) && i < for_col_node->num_child_; ++i) {
      ParseNode *for_col_item = NULL;
      ObSEArray<ObString, 4> for_col_list;
      MethodOptSizeConf size_conf(default_size_conf);
      if (OB_ISNULL(for_col_item = for_col_node->children_[i]) ||
          OB_UNLIKELY(for_col_item->type_ != T_FOR_COLUMN_ITEM || for_col_item->num_child_ != 2)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("for column item is null", K(ret), K(for_col_item));
      } else if (NULL == for_col_item->children_[0]) {
        if (i == 0) {
          if (NULL == for_col_item->children_[1]) {
            /*do nothing*/
          } else if (OB_FAIL(parse_size_clause(for_col_item->children_[1], default_size_conf))) {
            LOG_WARN("failed to parse size clause", K(ret));
          } else {/*do nothing*/}
        } else {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("get invalid syntax, can't parse", K(ret));
        }
      } else if (OB_UNLIKELY(T_EXTENSION == for_col_item->children_[0]->type_)) {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("does not support gather stats for multi columns", K(ret));
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "gather stats for multi columns");
      } else if (OB_FAIL(parse_for_columns(for_col_item->children_[0],
                                           column_params,
                                           for_col_list,
                                           record_cols))) {
        LOG_WARN("failed to parse for columns", K(ret));
      } else if (NULL == for_col_item->children_[1]) {
        if (i < for_col_node->num_child_ - 1) {
          ParseNode *tmp_for_col_item = NULL;
           if (OB_ISNULL(tmp_for_col_item = for_col_node->children_[i + 1]) ||
               OB_UNLIKELY(tmp_for_col_item->type_ != T_FOR_COLUMN_ITEM ||
                           tmp_for_col_item->num_child_ != 2)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("for column item is null", K(ret), K(for_col_item));
          } else if (tmp_for_col_item->children_[0] != NULL) {
            /*do nothing*/
          } else if (OB_FAIL(parse_size_clause(tmp_for_col_item->children_[1], size_conf))) {
            LOG_WARN("failed to parse size clause", K(ret));
          } else {
            ++ i;
          }
        }
      } else if (OB_FAIL(parse_size_clause(for_col_item->children_[1], size_conf))) {
        LOG_WARN("failed to parse size clause", K(ret));
      } else {/*do nothing*/}
      for (int64_t j = 0; OB_SUCC(ret) && j < column_params.count(); ++j) {
        ObColumnStatParam &col_param = column_params.at(j);
        if (!is_match_column_option(col_param, for_col_list)) {
          // do nothing
        } else if (!col_param.is_valid_opt_col()) {
          // do nothing
        } else if (OB_FAIL(compute_bucket_num(column_params.at(j), size_conf))) {
          LOG_WARN("failed to compute histogram size", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::compute_bucket_num(ObColumnStatParam &param,
                                    const MethodOptSizeConf &size_conf)
{
  int ret = OB_SUCCESS;
  param.set_need_basic_stat();
  if (size_conf.is_manual()) {
    param.set_size_manual();
    param.bucket_num_ = size_conf.val_;
  } else if (size_conf.is_auto()) {
    param.set_size_auto();
    param.column_usage_flag_ = 0;
    param.bucket_num_ = 1;
  } else if (size_conf.is_repeat()) {
    param.set_size_repeat();
    param.bucket_num_ = 1;
  } else if (size_conf.is_skewonly()) {
    param.set_size_skewonly();
    param.bucket_num_ = ObColumnStatParam::DEFAULT_HISTOGRAM_BUCKET_NUM;
  }
  return ret;
}

bool ObDbmsStats::is_match_column_option(ObColumnStatParam &param,
                                         const MethodOptColConf &for_all_opt)
{
  bool is_match = false;
  if (FOR_ALL == for_all_opt) {
    is_match = true;
  } else if (FOR_INDEXED == for_all_opt && param.is_index_column()) {
    is_match = true;
  } else if (FOR_HIDDEN == for_all_opt && param.is_hidden_column()) {
    is_match = true;
  }
  return is_match;
}

bool ObDbmsStats::is_match_column_option(ObColumnStatParam &param,
                                         const ObIArray<ObString> &for_col_list)
{
  bool is_match = false;
  for (int64_t i = 0; !is_match && i < for_col_list.count(); ++i) {
    if (0 == for_col_list.at(i).case_compare(param.column_name_)) {
      is_match = true;
    }
  }
  return is_match;
}

//specify number of histogram buckets. must be in the range [1,2048].
int ObDbmsStats::parse_size_clause(const ParseNode *node, MethodOptSizeConf &size_opt)
{
  int ret = OB_SUCCESS;
  int64_t value = 0;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid size clause", K(ret), K(node));
  } else if (node->type_ != T_INT && node->type_ != T_NUMBER) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("get invalid argument, expect INT or NUMBER type", K(ret), K(node->type_));
  } else if (node->type_ == T_INT) {
    value = node->value_;
  } else {
    number::ObNumber nmb;
    int16_t precision = PRECISION_UNKNOWN_YET;
    int16_t scale = SCALE_UNKNOWN_YET;
    ObArenaAllocator calc_buf(ObModIds::OB_SQL_PARSER);
    if (OB_FAIL(nmb.from(node->str_value_, static_cast<int32_t>(node->str_len_),
                         calc_buf, &precision, &scale))) {
      if (OB_INTEGER_PRECISION_OVERFLOW == ret) {
        LOG_WARN("integer presision overflow", K(ret));
      } else if (OB_NUMERIC_OVERFLOW == ret) {
        LOG_WARN("numeric overflow");
      } else {
        LOG_WARN("unexpected error", K(ret));
      }
    } else if (OB_FAIL(nmb.extract_valid_int64_with_round(value))) {
      LOG_WARN("failed to extract_valid_int64_with_round", K(ret), K(nmb));
    } else {/*do nothing*/}
  }
  if (OB_SUCC(ret)) {
    if (node->reserved_ == 1 && (value < 1 || value > 2048)) {
       ret = OB_ERR_INVALID_SIZE_SPECIFIED;
       LOG_WARN("get invalid argument, expected value in the range[1, 2048]", K(ret), K(value));
    } else {
      size_opt.mode_ = node->reserved_;
      size_opt.val_ = value;
    }
  }
  return ret;
}

int ObDbmsStats::parse_for_columns(const ParseNode *node,
                                   const ObIArray<ObColumnStatParam> &column_params,
                                   common::ObIArray<ObString> &cols,
                                   common::ObIArray<ObString> &record_cols)
{
  int ret = OB_SUCCESS;
  cols.reuse();
  if (OB_ISNULL(node) ||
      OB_UNLIKELY(node->type_ != T_COLUMN_REF || node->num_child_ != 3) ||
      OB_ISNULL(node->children_[1])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("node is null", K(ret), K(node));
  } else if (OB_FAIL(check_is_valid_col(node->children_[1]->str_value_, column_params, record_cols))) {
    LOG_WARN("failed to check is valid col", K(ret));
  } else if (OB_FAIL(cols.push_back(node->children_[1]->str_value_))) {
    LOG_WARN("failed to push back column", K(ret));
  } else if (OB_FAIL(record_cols.push_back(node->children_[1]->str_value_))) {
    LOG_WARN("failed to push back column", K(ret));
  }
  return ret;
}

int ObDbmsStats::check_is_valid_col(const ObString &src_str,
                                    const ObIArray<ObColumnStatParam> &column_params,
                                    const common::ObIArray<ObString> &record_cols)
{
  int ret = OB_SUCCESS;
  bool is_valid = false;
  //check col in table
  for (int64_t i = 0; !is_valid && i < column_params.count(); ++i) {
    if (0 == src_str.case_compare(column_params.at(i).column_name_)) {
      is_valid =  true;
    }
  }
  if (is_valid) {
    //check duplicate => dbms_stats.gather_table_stats('TEST', 'T1', method_opt=>'for columns c1 c1');
    for (int64_t i = 0; is_valid && i < record_cols.count(); ++i) {
      if (0 == src_str.case_compare(record_cols.at(i))) {
        is_valid = false;
      }
    }
    if (!is_valid) {
      ret = OB_ERR_COLUMN_DUPLICATE;
      LOG_WARN("column duplicated", K(src_str), K(ret));
      LOG_USER_ERROR(OB_ERR_COLUMN_DUPLICATE, src_str.length(), src_str.ptr());
    }
  } else {
    ret = OB_WRONG_COLUMN_NAME;
    LOG_WARN("column schema is null", K(ret), K(src_str));
    LOG_USER_ERROR(OB_WRONG_COLUMN_NAME, static_cast<int32_t>(src_str.length()), src_str.ptr());
  }
  return ret;
}

int ObDbmsStats::get_table_part_infos(const share::schema::ObTableSchema *table_schema,
                                      ObIArray<PartInfo> &part_infos,
                                      ObIArray<PartInfo> &subpart_infos,
                                      ObIArray<int64_t> &part_ids,
                                      ObIArray<int64_t> &subpart_ids,
                                      OSGPartMap *part_map/*default NULL*/)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(table_schema));
  } else if (!table_schema->is_partitioned_table()) {
    /*do notthing*/
    LOG_TRACE("table is not part table", K(table_schema->get_part_level()));
  } else if (OB_FAIL(ObDbmsStatsUtils::get_part_infos(*table_schema,
                                                      part_infos,
                                                      subpart_infos,
                                                      part_ids,
                                                      subpart_ids,
                                                      part_map))) {
    LOG_WARN("failed to get partition infos", K(ret));
  }
  return ret;
}

int ObDbmsStats::get_part_ids_from_schema(const ObTableSchema *table_schema,
                                          common::ObIArray<ObObjectID> &target_part_ids)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexptect null pointer", K(ret));
  } else {
    if (!table_schema->is_partitioned_table()) {
      if (OB_FAIL(target_part_ids.push_back(table_schema->get_object_id()))) {
        LOG_WARN("fail to push back part id", K(ret));
      }
    } else {
      ObSEArray<PartInfo, 4> dummy_part_infos;
      ObSEArray<PartInfo, 4> dummy_subpart_infos;
      ObSEArray<int64_t, 4> part_ids;
      ObSEArray<int64_t, 4> subpart_ids;
      if (OB_FAIL(get_table_part_infos(table_schema,
                                       dummy_part_infos,
                                       dummy_subpart_infos,
                                       part_ids,
                                       subpart_ids))) {
        LOG_WARN("fail to get part infos", K(ret));
      } else if (OB_FAIL(append(target_part_ids, part_ids))) {
        LOG_WARN("fail to append target part id", K(ret));
      } else if (OB_FAIL(append(target_part_ids, subpart_ids))) {
        LOG_WARN("fail to append target part id", K(ret));
      }
    }
  }
  return ret;
}

/*
* epc          NUMBER    DEFAULT NULL, Number of buckets in histogram
* minval       RAW       DEFAULT NULL, Minimum value
* maxval       RAW       DEFAULT NULL, Maximum value
* bkvals       NUMARRAY  DEFAULT NULL, Array of bucket numbers
* novals       NUMARRAY  DEFAULT NULL, Array of normalized end point values
* chvals       CHARARRAY DEFAULT NULL, Array of dumped end point values
* eavals       RAWARRAY  DEFAULT NULL, Array of end point actual values
* rpcnts       NUMARRAY  DEFAULT NULL, Array of end point value frequencies
* eavs         NUMBER    DEFAULT NULL, A number indicating whether actual end point values are needed
                                       in the histogram. If using the PREPARE_COLUMN_VALUES Procedures,
                                       this field will be automatically filled.
*
*/
int ObDbmsStats::parse_set_hist_stats_options(ObExecContext &ctx,
                                              const ObObjParam &epc,
                                              const ObObjParam &minval,
                                              const ObObjParam &maxval,
                                              const ObObjParam &bkvals,
                                              const ObObjParam &novals,
                                              const ObObjParam &chvals,
                                              const ObObjParam &eavals,
                                              const ObObjParam &rpcnts,
                                              const ObObjParam &eavs,
                                              ObHistogramParam &hist_param)
{
  int ret = OB_SUCCESS;
  UNUSED(ctx);
  number::ObNumber num_epc;
  number::ObNumber num_eavs;
  if (!epc.is_null() && OB_FAIL(epc.get_number(num_epc))) {
    LOG_WARN("failed to get epc", K(ret));
  } else if (!minval.is_null() && OB_FAIL(minval.get_raw(hist_param.minval_))) {
    LOG_WARN("failed to get minval", K(ret));
  } else if (!maxval.is_null() && OB_FAIL(maxval.get_raw(hist_param.maxval_))) {
    LOG_WARN("failed to get maxval", K(ret));
  } else if (OB_FAIL(parser_pl_numarray(bkvals, hist_param.bkvals_))) {
    LOG_WARN("failed to parser pl numarray", K(ret));
  } else if (OB_FAIL(parser_pl_numarray(novals, hist_param.novals_))) {
    LOG_WARN("failed to parser pl numarray", K(ret));
  } else if (OB_FAIL(parser_pl_chararray(chvals, hist_param.chvals_))) {
    LOG_WARN("failed to parser pl chararray", K(ret));
  } else if (OB_FAIL(parser_pl_rawarray(eavals, hist_param.eavals_))) {
    LOG_WARN("failed to parser pl rawarray", K(ret));
  } else if (OB_FAIL(parser_pl_numarray(rpcnts, hist_param.rpcnts_))) {
    LOG_WARN("failed to parser pl numarray", K(ret));
  } else if (!eavs.is_null() && OB_FAIL(eavs.get_number(num_eavs))) {
    LOG_WARN("failed to get eavs", K(ret));
  } else if (OB_FAIL(num_epc.extract_valid_int64_with_trunc(hist_param.epc_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_epc));
  } else if (OB_FAIL(num_eavs.extract_valid_int64_with_trunc(hist_param.eavs_))) {
    LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_eavs));
  }
  return ret;
}

int ObDbmsStats::parser_pl_numarray(const ObObjParam &numarray_param,
                                    ObIArray<int64_t> &num_array)
{
  int ret = OB_SUCCESS;
  ObObj *obj = NULL;
  if (!numarray_param.is_null()) {
    if (OB_LIKELY(numarray_param.is_ext())) {
      pl::ObPLCollection *numarray_ext = reinterpret_cast<pl::ObPLCollection *>(numarray_param.get_ext());
      if (OB_ISNULL(numarray_ext)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid argument", K(ret), K(numarray_ext));
      } else if (numarray_ext->is_collection_null()) {
        //do nothing
      } else if ((numarray_ext->get_count() != 0 &&
                  OB_ISNULL(obj = reinterpret_cast<ObObj *>(numarray_ext->get_data()))) ||
                 OB_UNLIKELY(pl::PL_VARRAY_TYPE != numarray_ext->get_type() ||
                             !(numarray_ext->is_inited()))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("get invalid argument", K(ret), K(numarray_ext), K(obj), K(numarray_ext->get_type()),
                                         K(numarray_ext->is_inited()), K(numarray_ext->get_count()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < numarray_ext->get_count(); ++i) {
          number::ObNumber num_elem;
          int64_t num;
          if (OB_FAIL(obj[i].get_number(num_elem))) {
            LOG_WARN("failed to get number", K(obj[i]), K(ret));
          } else if (OB_FAIL(num_elem.extract_valid_int64_with_trunc(num))) {
            LOG_WARN("extract_valid_int64_with_trunc failed", K(ret), K(num_elem));
          } else if (OB_FAIL(num_array.push_back(num))) {
            LOG_WARN("failed to push back num", K(ret));
          } else {/*do nothing*/}
        }
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get invalid argument", K(numarray_param));
    }
  }
  return ret;
}

int ObDbmsStats::parser_pl_chararray(const ObObjParam &chararray_param,
                                     ObIArray<ObString> &char_array)
{
  int ret = OB_SUCCESS;
  ObObj *obj = NULL;
  if (!chararray_param.is_null()) {
    if (OB_LIKELY(chararray_param.is_ext())) {
      pl::ObPLCollection *chararray_ext = reinterpret_cast<pl::ObPLCollection *>(chararray_param.get_ext());
      if (OB_ISNULL(chararray_ext)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid argument", K(ret), K(chararray_ext));
      } else if (chararray_ext->is_collection_null()) {
        //do nothing
      } else if ((chararray_ext->get_count() != 0 &&
                  OB_ISNULL(obj = reinterpret_cast<ObObj *>(chararray_ext->get_data()))) ||
                OB_UNLIKELY(pl::PL_VARRAY_TYPE != chararray_ext->get_type() ||
                            !(chararray_ext->is_inited()))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("get invalid argument", K(ret), K(obj), K(chararray_ext->get_type()),
                                        K(chararray_ext->get_count()), K(chararray_ext->is_inited()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < chararray_ext->get_count(); ++i) {
          ObString str;
          if (OB_FAIL(obj[i].get_string(str))) {
            LOG_WARN("failed to get number", K(obj[i]), K(ret));
          } else if (OB_FAIL(char_array.push_back(str))) {
            LOG_WARN("failed to push back num", K(ret));
          } else {/*do nothing*/}
        }
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(chararray_param));
    }
  }
  return ret;
}

int ObDbmsStats::parser_pl_rawarray(const ObObjParam &rawarray_param,
                                    ObIArray<ObString> &raw_array)
{
  int ret = OB_SUCCESS;
  ObObj *obj = NULL;
  if (!rawarray_param.is_null()) {
    if (OB_LIKELY(rawarray_param.is_ext())) {
      pl::ObPLCollection *rawarray_ext = reinterpret_cast<pl::ObPLCollection *>(rawarray_param.get_ext());
      if (OB_ISNULL(rawarray_ext)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get invalid argument", K(ret), K(rawarray_ext));
      } else if (rawarray_ext->is_collection_null()) {
        //do nothing
      } else if ((rawarray_ext->get_count() != 0 &&
                  OB_ISNULL(obj = reinterpret_cast<ObObj *>(rawarray_ext->get_data()))) ||
                 OB_UNLIKELY(pl::PL_VARRAY_TYPE != rawarray_ext->get_type() ||
                            !(rawarray_ext->is_inited()))) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("get invalid argument", K(ret), K(obj), K(rawarray_ext->get_type()),
                                        K(rawarray_ext->get_count()), K(rawarray_ext->is_inited()));
      } else {
        for (int64_t i = 0; OB_SUCC(ret) && i < rawarray_ext->get_count(); ++i) {
          ObString raw;
          if (OB_FAIL(obj[i].get_raw(raw))) {
            LOG_WARN("failed to get number", K(obj[i]), K(ret));
          } else if (OB_FAIL(raw_array.push_back(raw))) {
            LOG_WARN("failed to push back num", K(ret));
          } else {/*do nothing*/}
        }
      }
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("get invalid argument", K(rawarray_param));
    }
  }
  return ret;
}

int ObDbmsStats::find_selected_part_infos(const ObString &part_name,
                                          const ObIArray<PartInfo> &part_infos,
                                          const ObIArray<PartInfo> &subpart_infos,
                                          const bool is_sensitive_compare,
                                          ObIArray<PartInfo> &new_part_infos,
                                          ObIArray<PartInfo> &new_subpart_infos,
                                          bool &is_subpart_name)
{
  int ret = OB_SUCCESS;
  bool found = false;
  PartInfo part;
  is_subpart_name = false;
  bool is_twopart = false;
  for (int64_t i = 0; !found && i < part_infos.count(); ++i) {
    if ((is_sensitive_compare &&
         ObCharset::case_sensitive_equal(part_name, part_infos.at(i).part_name_)) ||
        (!is_sensitive_compare &&
         ObCharset::case_insensitive_equal(part_name, part_infos.at(i).part_name_))) {
      part = part_infos.at(i);
      found = true;
    }
  }
  for (int64_t i = 0; !found && i < subpart_infos.count(); ++i) {
    if ((is_sensitive_compare &&
         ObCharset::case_sensitive_equal(part_name, subpart_infos.at(i).part_name_)) ||
        (!is_sensitive_compare &&
         ObCharset::case_insensitive_equal(part_name, subpart_infos.at(i).part_name_))) {
      part = subpart_infos.at(i);
      found = true;
      is_twopart = true;
    }
  }
  if (OB_SUCC(ret)) {
    if (!found) {
      ret = OB_UNKNOWN_PARTITION;
      LOG_WARN("the specified partition is not found", K(ret), K(part_name));
    } else if (is_twopart) {
      if (OB_FAIL(new_subpart_infos.push_back(part))) {
        LOG_WARN("failed to push back part info", K(ret));
      } else {
        bool find_it = false;
        int64_t cur_part_id = part.first_part_id_;
        for (int64_t i = 0; OB_SUCC(ret) && !find_it && i < part_infos.count(); ++i) {
          if (cur_part_id != part_infos.at(i).part_id_) {
            // do nothing
          } else if (OB_FAIL(new_part_infos.push_back(part_infos.at(i)))) {
            LOG_WARN("failed to push back subpart infos", K(ret));
          } else {
            find_it = true;
          }
        }
        if (OB_SUCC(ret) && !find_it) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("get unexpected error, partition id isn't found", K(ret), K(cur_part_id));
        } else {
          is_subpart_name = true;
        }
      }
    } else {
      if (OB_FAIL(new_part_infos.push_back(part))) {
        LOG_WARN("failed to push back part info", K(ret));
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < subpart_infos.count(); ++i) {
        int64_t cur_part_id = subpart_infos.at(i).first_part_id_;
        if (cur_part_id != part.part_id_) {
          // do nothing
        } else if (OB_FAIL(new_subpart_infos.push_back(subpart_infos.at(i)))) {
          LOG_WARN("failed to push back subpart infos", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::process_not_size_manual_column(sql::ObExecContext &ctx,
                                                ObTableStatParam &table_param)
{
  int ret = OB_SUCCESS;
  ObSEArray<ObColumnStatParam *, 8> auto_columns;
  ObSEArray<ObColumnStatParam *, 8> repeat_columns;
  ObSEArray<uint64_t, 8> column_ids;

  for (int64_t i = 0; OB_SUCC(ret) && i < table_param.column_params_.count(); ++i) {
    if (table_param.column_params_.at(i).is_size_auto()) {
      if (OB_FAIL(auto_columns.push_back(&table_param.column_params_.at(i)))) {
        LOG_WARN("failed to push back column param", K(ret));
      }
    } else if (table_param.column_params_.at(i).is_size_repeat()) {
      if (OB_FAIL(repeat_columns.push_back(&table_param.column_params_.at(i)))) {
        LOG_WARN("failed to push back column param", K(ret));
      } else if (OB_FAIL(column_ids.push_back(table_param.column_params_.at(i).column_id_))) {
        LOG_WARN("failed to push back column id", K(ret));
      }
    }
  }
  if (OB_SUCC(ret) && !auto_columns.empty()) {
    if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx, true, false))) {
      LOG_WARN("failed to do flush database monitoring info", K(ret));
    } else if (OB_FAIL(ObOptStatMonitorManager::get_instance().get_column_usage_from_table(
                ctx, auto_columns, table_param.tenant_id_, table_param.table_id_))) {
      LOG_WARN("failed to get column usage from table", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < auto_columns.count(); ++i) {
        int64_t flag = auto_columns.at(i)->column_usage_flag_;
        // do not create histogram if a unique column only has equality predicates
        if (auto_columns.at(i)->is_unique_column()) {
          flag &= ~EQUALITY_PREDS;
          flag &= ~EQUIJOIN_PREDS;
        }
        // do not create histogram if a not null column only has null predicates
        if (auto_columns.at(i)->is_not_null_column()) {
          flag &= ~NULL_PREDS;
        }
        if (flag > 0) {
          auto_columns.at(i)->bucket_num_ = ObColumnStatParam::DEFAULT_HISTOGRAM_BUCKET_NUM;
        }
      }
    }
  }
  if (OB_SUCC(ret) && !repeat_columns.empty()) {
    ObSEArray<int64_t, 1> part_ids;
    const int64_t part_id = table_param.global_part_id_;
    ObArray<ObOptColumnStatHandle> stat_handles;
    if (OB_FAIL(part_ids.push_back(part_id))) {
      LOG_WARN("failed to push back part id", K(ret));
    } else if (OB_FAIL(ObOptStatManager::get_instance().get_column_stat(table_param.tenant_id_,
                                                                        table_param.table_id_,
                                                                        part_ids,
                                                                        column_ids,
                                                                        stat_handles))) {
      LOG_WARN("failed to get column stat", K(ret));
    } else if (OB_UNLIKELY(stat_handles.count() != repeat_columns.count())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("number of stat handle and column param should equal",
                  K(ret), K(stat_handles.count()), K(repeat_columns.count()));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < stat_handles.count(); ++i) {
        const ObOptColumnStat *opt_col_stat = NULL;
        if (OB_ISNULL(opt_col_stat = stat_handles.at(i).stat_)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("cache value is null", K(ret));
        } else if (opt_col_stat->get_histogram().is_valid()) {
          bool found_it = false;
          for (int64_t j = 0; !found_it && j < repeat_columns.count(); ++j) {
            if (repeat_columns.at(j)->column_id_ == opt_col_stat->get_column_id()) {
              repeat_columns.at(j)->bucket_num_ = opt_col_stat->get_histogram().get_bucket_size();
              found_it = true;
            }
          }
          if (OB_UNLIKELY(!found_it)) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected error", K(repeat_columns), KPC(opt_col_stat), K(ret));
          }
        }
      }
    }
  }
  return ret;
}

int ObDbmsStats::flush_database_monitoring_info(sql::ObExecContext &ctx,
                                                sql::ParamStore &params,
                                                common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(params);
  UNUSED(result);
  bool is_flush_col_usage = true;
  bool is_flush_dml_stat = true;
  bool ignore_failed = false;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx,
                                                                             is_flush_col_usage,
                                                                             is_flush_dml_stat,
                                                                             ignore_failed))) {
    LOG_WARN("failed to do flush database monitoring info", K(ret));
  }
  return ret;
}

// inner table related to statistics can not read/write during physical restore, and can not write
// on standby cluster. So any dbms_stats interface need read or write these tables should call this
// function to check status.
int ObDbmsStats::check_statistic_table_writeable(sql::ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  bool in_restore = false;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(ctx.get_my_session())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(schema_guard));
  } else if (OB_FAIL(schema_guard->check_tenant_is_restore(ctx.get_my_session()->get_effective_tenant_id(),
                                                           in_restore))) {
    LOG_WARN("failed to check tenant is restore", K(ret));
  } else if (OB_UNLIKELY(in_restore) ||
             GCTX.is_standby_cluster()) {
    ret = OB_NOT_SUPPORTED;
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "use dbms_stats during restore or standby cluster");
  }
  return ret;
}

//Maybe following code will be useful in the future, keep it temporarily.
//gather granularity depending on partition type
// int ObDbmsStats::set_default_gather_granularity(share::schema::ObPartitionLevel part_level,
//                                                 bool &need_global,
//                                                 bool &need_approx_global,
//                                                 bool &need_part,
//                                                 bool &need_subpart)
// {
//   int ret = OB_SUCCESS;
//   if (part_level == share::schema::PARTITION_LEVEL_ZERO) {
//     need_global = true;
//     need_approx_global = false;
//     need_part = false;
//     need_subpart = false;
//   } else if (part_level == share::schema::PARTITION_LEVEL_ONE) {
//     need_global = false;
//     need_approx_global = true;
//     need_part = true;
//     need_subpart = false;
//   } else if (part_level == share::schema::PARTITION_LEVEL_TWO) {
//     need_global = false;
//     need_approx_global = true;
//     need_part = false;
//     need_subpart = true;
//   } else {
//     ret = OB_ERR_UNEXPECTED;
//     LOG_WARN("get unexpected error, expected valid part level", K(part_level));
//   }
//   return ret;
// }

int ObDbmsStats::parse_column_info(sql::ObExecContext &ctx,
                                   const ObObjParam &column_name,
                                   ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  ObString col_name;
  if (OB_ISNULL(param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(param));
  } else if (column_name.is_null()) {
    /*do nothing*/
  } else if (OB_FAIL(column_name.get_varchar(col_name))) {
    LOG_WARN("failed to get varchar", K(ret));
  } else if (OB_FAIL(convert_vaild_ident_name(*param.allocator_,
                                              ctx.get_my_session()->get_dtc_params(),
                                              col_name,
                                              lib::is_oracle_mode()))) {
    LOG_WARN("failed to convert vaild ident name", K(ret));
  } else {
    ObSEArray<ObColumnStatParam, 1> new_col_params;
    bool find_it = false;
    for (int64_t i = 0; OB_SUCC(ret) && !find_it && i < param.column_params_.count(); ++i) {
      if ((lib::is_oracle_mode() &&
           ObCharset::case_sensitive_equal(col_name, param.column_params_.at(i).column_name_)) ||
          (!lib::is_oracle_mode() &&
           ObCharset::case_insensitive_equal(col_name, param.column_params_.at(i).column_name_))) {
        if (OB_FAIL(new_col_params.push_back(param.column_params_.at(i)))) {
          LOG_WARN("failed to push back column params", K(ret));
        } else {
          find_it = true;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (!find_it) {
        ret = OB_ERR_DBMS_STATS_PL;
        LOG_WARN("invalid column name", K(ret), K(column_name), K(col_name), K(param));
        LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "invalid column name");
      } else if (OB_FAIL(param.column_params_.assign(new_col_params))) {
        LOG_WARN("failed to assign column params", K(ret));
      } else {
        LOG_TRACE("succeed to parse export column info", K(col_name));
      }
    }
  }
  return ret;
}

/*this option just compatible oracle syntax*/
int ObDbmsStats::parse_stat_category(const ObString &stat_category)
{
  int ret = OB_SUCCESS;
  if (stat_category.empty()) {
    /*do nothing*/
  } else {
    LOG_TRACE("begin parse stat category", K(stat_category));
    const char *ptr = stat_category.ptr();
    const char *tmp_ptr = ptr;
    int64_t str_len = 0;
    bool is_object_stats = false;//table statistics, column statistics and index statistics(Default)
    bool is_synopses = false;//information to support incremental statistics
    for (int64_t i = 0; OB_SUCC(ret) && i < stat_category.length(); ++i) {
      if (ptr[i] == ' ' || ptr[i] == ',') {
        ObString tmp_str(str_len, tmp_ptr);
        if (tmp_str.empty()) {
          /*do nothing*/
        } else if (0 == tmp_str.case_compare("OBJECT_STATS")) {
          is_object_stats = true;
          tmp_ptr = NULL;
          str_len = 0;
        } else if (0 == tmp_str.case_compare("SYNOPSES")) {
          is_synopses = true;
          tmp_ptr = NULL;
          str_len = 0;
        } else {
          ret = OB_ERR_DBMS_STATS_PL;
          LOG_WARN("invalid stat category", K(ret));
          LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "invalid stat category");
        }
      } else {
        ++ str_len;
        if (tmp_ptr == NULL) {
          tmp_ptr = ptr + i;
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (str_len == 0 || tmp_ptr == NULL) {
        ret = OB_ERR_DBMS_STATS_PL;
        LOG_WARN("invalid stat category", K(ret));
        LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "invalid stat category");
      } else {
        ObString tmp_str(str_len, tmp_ptr);
        if (0 == tmp_str.case_compare("OBJECT_STATS")) {
          is_object_stats = true;
        } else if (0 == tmp_str.case_compare("SYNOPSES")) {
          is_synopses = true;;
        } else {
          ret = OB_ERR_DBMS_STATS_PL;
          LOG_WARN("invalid stat category", K(ret));
          LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "invalid stat category");
        }
      }
    }
    if (OB_SUCC(ret)) {
      if (!is_object_stats) {
        ret = OB_ERR_DBMS_STATS_PL;
        LOG_WARN("invalid stat category: OBJECT_STATS not included", K(ret));
        LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "invalid stat category: OBJECT_STATS not included");
      }
    }
  }
  return ret;
}

int ObDbmsStats::parse_stat_type(const ObString &stat_type_str,
                                 StatTypeLocked &stat_type)
{
  int ret = OB_SUCCESS;
  if (0 == stat_type_str.case_compare("ALL")) {
    stat_type = StatTypeLocked::TABLE_ALL_TYPE;
  } else if (0 == stat_type_str.case_compare("DATA")) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("dbms_stats with DATA STATTYPE_LOCKED not support", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "dbms_stats with DATA STATTYPE_LOCKED");
  } else if (0 == stat_type_str.case_compare("CACHE")) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("dbms_stats with CACHE STATTYPE_LOCKED not support", K(ret));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "dbms_stats with CACHE STATTYPE_LOCKED");
  } else {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("Invalid or inconsistent input values", K(ret), K(stat_type_str));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid or inconsistent input values");
  }
  return ret;
}

int ObDbmsStats::get_all_table_ids_in_database(ObExecContext &ctx,
                                               const ObObjParam &owner,
                                               ObTableStatParam &stat_param,
                                               ObIArray<uint64_t> &table_ids)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = ctx.get_my_session();
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  ObSEArray<const ObTableSchema *, 4> table_schemas;
  if (OB_ISNULL(session) || OB_ISNULL(schema_guard) || OB_ISNULL(stat_param.allocator_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("params have null", K(ret), K(session), K(schema_guard), K(stat_param.allocator_));
  } else {
    stat_param.tenant_id_ = session->get_effective_tenant_id();
    if (owner.is_null()) {
      stat_param.db_name_ = session->get_user_name();
    } else if (OB_FAIL(owner.get_string(stat_param.db_name_))) {
      LOG_WARN("failed to get db name", K(ret));
    } else if (OB_FAIL(convert_vaild_ident_name(*stat_param.allocator_,
                                                session->get_dtc_params(),
                                                stat_param.db_name_,
                                                lib::is_oracle_mode()))) {
      LOG_WARN("failed to convert vaild ident name", K(ret));
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(schema_guard->get_database_id(stat_param.tenant_id_,
                                                stat_param.db_name_,
                                                stat_param.db_id_))) {
        LOG_WARN("failed to get database id", K(ret));
      } else if (OB_UNLIKELY(OB_INVALID_ID == stat_param.db_id_)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("schema is not exist", K(ret), K(stat_param.db_name_), K(stat_param.db_id_));
      } else if (OB_FAIL(schema_guard->get_table_schemas_in_database(stat_param.tenant_id_,
                                                                     stat_param.db_id_,
                                                                     table_schemas))) {
        LOG_WARN("failed to get table schemas in database", K(ret));
      } else {
        bool is_valid = false;
        for (int64_t i = 0; OB_SUCC(ret) && i < table_schemas.count(); ++i) {
          if (OB_ISNULL(table_schemas.at(i))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret));
          } else if (OB_FAIL(ObDbmsStatsUtils::check_is_stat_table(*schema_guard,
                                                                   stat_param.tenant_id_,
                                                                   table_schemas.at(i)->get_table_id(),
                                                                   is_valid))) {
            LOG_WARN("failed to check is stat table", K(ret));
          } else if (!is_valid) {
            // only need following tables:
            // 1. user table
            // 2. valid sys table
            // 3. valid virtual table
          } else if (share::is_oracle_mapping_real_virtual_table(table_schemas.at(i)->get_table_id())
                     && table_schemas.at(i)->is_index_table()) {
            // skip
          } else if (OB_FAIL(table_ids.push_back(table_schemas.at(i)->get_table_id()))) {
            LOG_WARN("failed to push back id", K(ret));
          } else {/*do nothing*/}
        }
        LOG_TRACE("succeed to get all table ids", K(table_ids), K(table_schemas));
      }
    }
  }
  return ret;
}

int ObDbmsStats::gather_database_stats_job_proc(sql::ObExecContext &ctx,
                                                sql::ParamStore &params,
                                                common::ObObj &result)
{
  int ret = OB_SUCCESS;
  UNUSED(result);
  const int64_t start_time = ObTimeUtility::current_time();
  ObOptStatTaskInfo task_info;
  ObGatherTableStatsHelper helper;
  if (OB_FAIL(check_statistic_table_writeable(ctx))) {
    ret = OB_SUCCESS;
    LOG_INFO("auto gather database statistics abort because of statistic table is unwriteable");
  } else if (OB_FAIL(ObOptStatMonitorManager::flush_database_monitoring_info(ctx))) {
    LOG_WARN("failed to flush database monitoring info", K(ret));
  } else if (OB_FAIL(helper.get_duration_time(params))) {
    LOG_WARN("failed to get duration time");
  } else if (OB_FAIL(get_need_statistics_tables(ctx, helper))) {
    LOG_WARN("failed to get need statistics tables", K(ret));
  } else if (helper.need_gather_table_stats()) {
    int64_t total_cnt = helper.stat_tables_.count();
    if (OB_FAIL(init_gather_task_info(ctx, ObOptStatGatherType::AUTO_GATHER, start_time, total_cnt, task_info))) {
      LOG_WARN("failed to init gather task info", K(ret));
    } else if (OB_FAIL(gather_tables_stats_with_default_param(ctx, helper, task_info))) {
      LOG_WARN("failed to gather tables tats with default param");
    } else {/*do nothing*/}
    const int64_t exe_time = ObTimeUtility::current_time() - start_time;
    LOG_INFO("have been gathered database stats job",
              "the total used time:", exe_time,
              "the duration time:", helper.duration_time_,
              "the toatal gather table cnt:", total_cnt,
              "the succeed to gather table cnt:", helper.succeed_count_,
              "the failed to gather table cnt:", helper.failed_count_, K(ret));
    //reset the error code, the reason is that the total gather time is reach the duration time.
    ret = ret == OB_TIMEOUT ? OB_SUCCESS : ret;
    task_info.task_end_time_ = ObTimeUtility::current_time();
    task_info.ret_code_ = ret;
    task_info.failed_count_ = helper.failed_count_;
    ObOptStatManager::get_instance().update_opt_stat_task_stat(task_info);
  }
  return ret;
}

int ObDbmsStats::get_need_statistics_tables(sql::ObExecContext &ctx, ObGatherTableStatsHelper &helper)
{
  int ret = OB_SUCCESS;
  ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  ObSQLSessionInfo *session = ctx.get_my_session();
  ObSEArray<const ObDatabaseSchema *, 16> database_schemas;
  uint64_t tenant_id = OB_INVALID_ID;
  uint64_t database_id = OB_INVALID_ID;
  const ObDatabaseSchema *database_schema = NULL;
  const ObTableSchema *table_schema = NULL;
  if (OB_ISNULL(schema_guard) || OB_ISNULL(session)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(schema_guard), K(session));
  } else if (OB_FALSE_IT(tenant_id = session->get_effective_tenant_id())) {
  } else if (is_virtual_tenant_id(tenant_id)) {
    // do nothing
  } else if (OB_FAIL(schema_guard->get_database_schemas_in_tenant(tenant_id, database_schemas))) {
    LOG_WARN("failed to get database sehcmas in tenant", K(ret));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < database_schemas.count(); ++i) {
      ObSEArray<const ObTableSchema *, 128> table_schemas;
      if (OB_ISNULL(database_schema = database_schemas.at(i))) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), K(database_schema));
      } else if (is_recyclebin_database_id(database_id = database_schema->get_database_id())) {
        // do not gather statistics for tables in recyclebin
      } else if (OB_FAIL(schema_guard->get_table_schemas_in_database(tenant_id,
                                                                     database_id,
                                                                     table_schemas))) {
        LOG_WARN("failed to get table schema in database", K(ret));
      } else {
        for (int64_t j = 0; OB_SUCC(ret) && j < table_schemas.count(); ++j) {
          bool is_valid = false;
          if (OB_ISNULL(table_schema = table_schemas.at(j))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("get unexpected null", K(ret), K(table_schema));
          } else if (OB_FAIL(ObDbmsStatsUtils::check_is_stat_table(*schema_guard,
                                                                   tenant_id,
                                                                   table_schema->get_table_id(),
                                                                   is_valid))) {
            LOG_WARN("failed to check sy table validity", K(ret));
          } else if (!is_valid || table_schema->is_external_table()) {
            // only gather statistics for following tables:
            // 1. user table
            // 2. valid sys table
            // 3. virtual table
          } else {
            ObStatTableWrapper wrapper;
            StatTable stat_table(database_id, table_schema->get_table_id());
            double stale_percent_threshold = OPT_DEFAULT_STALE_PERCENT;
            bool is_big_table = false;
            if (OB_FAIL(get_table_stale_percent_threshold(ctx,
                                                          tenant_id,
                                                          stat_table.table_id_,
                                                          stale_percent_threshold))) {
              LOG_WARN("failed to get table stale percent threshold", K(ret));
            } else if (OB_FAIL(get_table_stale_percent(ctx, tenant_id,
                                                       *table_schema,
                                                       stale_percent_threshold,
                                                       stat_table,
                                                       is_big_table))) {
              LOG_WARN("failed to get table stale percent", K(ret));
            } else if (OB_FAIL(ObBasicStatsEstimator::get_gather_table_duration(ctx,
                                                                                tenant_id,
                                                                                table_schema->get_table_id(),
                                                                                wrapper.last_gather_duration_))) {// Note:
              LOG_WARN("failed to get gather table duration");
            } else if (stat_table.stale_percent_ < 0) {
              wrapper.stat_type_ = ObStatType::ObFirstTimeToGather;
            } else if (stat_table.stale_percent_ > stale_percent_threshold) {
              wrapper.stat_type_ = ObStatType::ObStale;
            } else {
              wrapper.stat_type_ = ObStatType::ObNotStale;
            }
            if (OB_SUCC(ret)) {
              wrapper.table_type_ = table_schema->is_user_table() ?
                                    ObStatTableType::ObUserTable : ObStatTableType::ObSysTable;
              wrapper.is_big_table_ = is_big_table;
              if (wrapper.stat_type_ == ObStatType::ObNotStale) {
                // do nothing
              } else if (OB_FAIL(wrapper.stat_table_.assign(stat_table))) {
                LOG_WARN("failed to assign stat table");
              } else if (OB_FAIL(helper.stat_tables_.push_back(wrapper))) {
                LOG_WARN("failed to push back stat tables");
              }
            }
          }
        }
      }
    }
    if (OB_SUCC(ret) && !helper.stat_tables_.empty()) {
      std::sort(&helper.stat_tables_.at(0), &helper.stat_tables_.at(0) + helper.stat_tables_.count());
    }
  }
  return ret;
}

int ObDbmsStats::get_table_stale_percent(sql::ObExecContext &ctx,
                                         const uint64_t tenant_id,
                                         const ObTableSchema &table_schema,
                                         const double stale_percent_threshold,
                                         StatTable &stat_table,
                                         bool &is_big_table)
{
  int ret = OB_SUCCESS;
  uint64_t table_id = table_schema.get_table_id();
  const int64_t part_id = PARTITION_LEVEL_ZERO == table_schema.get_part_level() ? table_id : -1;
  ObSEArray<ObPartitionStatInfo, 4> partition_stat_infos;
  bool is_locked = false;
  is_big_table = false;
  if (OB_FAIL(ObBasicStatsEstimator::check_table_statistics_state(ctx,
                                                                  tenant_id,
                                                                  table_id,
                                                                  part_id,
                                                                  is_locked,
                                                                  partition_stat_infos))) {// Note:
    LOG_WARN("failed to check table has any statistics", K(ret));
  } else if (is_locked) {
    //if table is locked, don't gather stats.
    stat_table.incremental_stat_ = false;
    stat_table.stale_percent_ = 0;
  } else if (table_schema.is_user_table() && -1 == part_id) {//for partitioned user table
    if (OB_FAIL(get_user_partition_table_stale_percent(ctx, tenant_id, table_schema,
                                                       stale_percent_threshold,
                                                       partition_stat_infos,
                                                       stat_table,
                                                       is_big_table))) {
      LOG_WARN("faild to get user partition table stale percent", K(ret));
    } else {/*do nothing*/}
  } else if (OB_FAIL(get_common_table_stale_percent(ctx, tenant_id, table_schema,
                                                    partition_stat_infos, stat_table, is_big_table))) {
    LOG_WARN("failed to get common table stale percent", K(ret));
  } else {/*do nothing*/}
  return ret;
}

/*for system table and non-partitioned user table:
 * 1. if table do not have global statistics, gather whole statistics;
 * 2. if table has global statistics, but statistics is stale, gather whole statistics;
 * 3. otherwise, do not gather statistics
 */
int ObDbmsStats::get_common_table_stale_percent(sql::ObExecContext &ctx,
                                                const uint64_t tenant_id,
                                                const ObTableSchema &table_schema,
                                                const ObIArray<ObPartitionStatInfo> &partition_stat_infos,
                                                StatTable &stat_table,
                                                bool &is_big_table)
{
  int ret = OB_SUCCESS;
  is_big_table = false;
  //if this is virtual table real agent, we need see the real table id modifed count
  uint64_t table_id = share::is_oracle_mapping_real_virtual_table(table_schema.get_table_id()) ?
                                   share::get_real_table_mappings_tid(table_schema.get_table_id()) :
                                   table_schema.get_table_id();
  const int64_t part_id = PARTITION_LEVEL_ZERO == table_schema.get_part_level() ? table_id : -1;
  int64_t inc_modified_count = 0;
  stat_table.incremental_stat_ = false;
  int64_t row_cnt = 0;
  bool is_gather_global_stat = false;
  if (OB_UNLIKELY(table_schema.is_user_table() && -1 == part_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(table_schema.is_user_table()), K(part_id));
  } else if (!is_table_gather_global_stats(part_id, partition_stat_infos, row_cnt)) {
    stat_table.stale_percent_ = -1.0;
    if (OB_FAIL(ObBasicStatsEstimator::estimate_row_count(ctx,
                                                          tenant_id,
                                                          table_id,
                                                          row_cnt))) {// Note:
      LOG_WARN("failed to estimate row count");
    } else {
      // currently we only regard tables whose rows >= 1000w as big table
      is_big_table = row_cnt >= OPT_STATS_BIG_TABLE_ROWS;
    }
  } else if (is_virtual_table(table_id)) {//virtual table doesn't see the modfiy count, no need regather
    stat_table.stale_percent_ = 0.0;
  } else if (OB_FAIL(ObBasicStatsEstimator::estimate_modified_count(ctx,
                                                                    tenant_id,
                                                                    table_id,
                                                                    inc_modified_count))) {
    LOG_WARN("failed to estimate modified count", K(ret));
  } else if (inc_modified_count < 0) {
    // if some server reboot, increment modified count may less than 0. In this scenario,
    // we force gather table statistics and reset modified count.
    stat_table.stale_percent_ = -1.0;
  } else if (inc_modified_count == 0) {
    stat_table.stale_percent_ = 0.0;
  } else {
    stat_table.stale_percent_ = row_cnt == 0 ? 1.0 : 1.0 * (inc_modified_count) / row_cnt;
  }
  LOG_TRACE("succeed to get common table stale percent", K(stat_table), K(partition_stat_infos), K(is_big_table));
  return ret;
}

/*for partitioned user table:
 *  1.if table do not have global statistics:
 *    i.if table do not have any part level statistics, then gather whole statistics;
 *    ii.if table have part level statistics:
 *       a.if less than %50 of all part have stale statistics or not have statistics, then use
 *         incremental gather statistics, it's mean to just regather part level statistics which
 *         statistics is stale and derive global statistics according to all part level statistics;
 *       b.if more than %50 of all part have stale statistics or not have statistics, then gather
 *         whole statistics;
 *   2.if table have global statistics:
 *     i.if global statistics is stale:
 *       a.if less than %50 of all part have stale statistics or not have statistics, then use
 *         incremental gather statistics, it's mean to just regather part level statistics which
 *         statistics is stale and derive global statistics according to all part level statistics;
 *       b.if more than %50 of all part have stale statistics or not have statistics, gather whole
 *         statistics;
 *     ii.if global statistics is not stale:
 *        a.regather part statistics which have stale statistics or not have statistics. then use
 *          incremental gather statistics, it's mean to just regather part level statistics which
 *          statistics is stale and derive global statistics according to all part level statistics;
 */
int ObDbmsStats::get_user_partition_table_stale_percent(
    sql::ObExecContext &ctx,
    const uint64_t tenant_id,
    const ObTableSchema &table_schema,
    const double stale_percent_threshold,
    const ObIArray<ObPartitionStatInfo> &partition_stat_infos,
    StatTable &stat_table,
    bool &is_big_table)
{
  int ret = OB_SUCCESS;
  is_big_table = false;
  uint64_t table_id = table_schema.get_table_id();
  const int64_t part_id = PARTITION_LEVEL_ZERO == table_schema.get_part_level() ? table_id : -1;
  int64_t inc_modified_count = 0;
  int64_t row_cnt = 0;
  ObSEArray<int64_t, 4> no_regather_partition_ids;
  int64_t no_regather_first_part_cnt = 0;
  ObSEArray<PartInfo, 4> partition_infos;
  if (OB_UNLIKELY(!table_schema.is_user_table() || -1 != part_id)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected error", K(ret), K(table_schema.is_user_table()), K(part_id));
  } else if (partition_stat_infos.empty()) {
    // do not have any statistics
    stat_table.stale_percent_ = -1.0;
    stat_table.incremental_stat_ = false;
    if (OB_FAIL(ObBasicStatsEstimator::estimate_row_count(ctx,
                                                          tenant_id,
                                                          table_id,
                                                          row_cnt))) {// Note:
      LOG_WARN("failed to estimate row count");
    } else {
      is_big_table = row_cnt >= OPT_STATS_BIG_TABLE_ROWS;
    }
  } else if (OB_FAIL(get_table_partition_infos(table_schema, partition_infos))) {
    LOG_WARN("failed to get table subpart infos", K(ret));
  } else if (!is_table_gather_global_stats(part_id, partition_stat_infos, row_cnt)) {
  //do not have global statistics, but have part level statistics
    stat_table.stale_percent_ = -1.0;
    if (OB_FAIL(ObBasicStatsEstimator::estimate_stale_partition(ctx,
                                                                tenant_id,
                                                                table_id,
                                                                partition_infos,
                                                                stale_percent_threshold,
                                                                partition_stat_infos,
                                                                no_regather_partition_ids,
                                                                no_regather_first_part_cnt))) {
      LOG_WARN("failed to get no regather partition", K(ret));
    } else if (no_regather_first_part_cnt > table_schema.get_first_part_num() / 2) {
      if (OB_FAIL(append(stat_table.no_regather_partition_ids_, no_regather_partition_ids))) {
        LOG_WARN("failed to append table ids");
      } else {
        stat_table.incremental_stat_ = true;
        stat_table.need_gather_subpart_ = PARTITION_LEVEL_TWO == table_schema.get_part_level();
      }
    } else {
      stat_table.incremental_stat_ = false;
    }
  //have global statistics
  } else if (OB_FAIL(ObBasicStatsEstimator::estimate_modified_count(ctx,
                                                                    tenant_id,
                                                                    table_id,
                                                                    inc_modified_count))) {// Note:
    LOG_WARN("failed to estimate modified count", K(ret));
  } else if (inc_modified_count < 0) {
    // if some server reboot, increment modified count may less than 0. In this scenario,
    // we force gather table statistics and reset modified count.
    stat_table.stale_percent_ = -1.0;
    stat_table.incremental_stat_ = false;
  } else if (inc_modified_count == 0) {
    stat_table.stale_percent_ = 0.0;
    stat_table.incremental_stat_ = false;
  } else {
    stat_table.stale_percent_ = row_cnt == 0 ? 1.0 : 1.0 * (inc_modified_count) / row_cnt;
    if (stat_table.stale_percent_ > stale_percent_threshold) {//global stat is stale
      stat_table.incremental_stat_ = false;
      if (OB_FAIL(ObBasicStatsEstimator::estimate_stale_partition(ctx,
                                                                  tenant_id,
                                                                  table_id,
                                                                  partition_infos,
                                                                  stale_percent_threshold,
                                                                  partition_stat_infos,
                                                                  no_regather_partition_ids,
                                                                  no_regather_first_part_cnt))) {
        LOG_WARN("failed to estimate stale partition", K(ret));
      } else if (no_regather_first_part_cnt > table_schema.get_first_part_num() / 2) {
        if (OB_FAIL(append(stat_table.no_regather_partition_ids_, no_regather_partition_ids))) {
          LOG_WARN("failed to append table ids");
        } else {
          stat_table.incremental_stat_ = true;
          stat_table.need_gather_subpart_ = PARTITION_LEVEL_TWO == table_schema.get_part_level();
        }
      } else {
        stat_table.incremental_stat_ = false;
      }
    } else if (OB_FAIL(ObBasicStatsEstimator::estimate_stale_partition(ctx,
                                                                       tenant_id,
                                                                       table_id,
                                                                       partition_infos,
                                                                       stale_percent_threshold,
                                                                       partition_stat_infos,
                                                                       no_regather_partition_ids,
                                                                       no_regather_first_part_cnt))) {// Note:
      LOG_WARN("failed to estimate stale partition", K(ret));
    } else {
      int64_t total_part_cnt = table_schema.get_all_part_num();
      if (PARTITION_LEVEL_TWO == table_schema.get_part_level()) {
        total_part_cnt += table_schema.get_first_part_num();
      }
      if (no_regather_partition_ids.count() < total_part_cnt ||
          is_all_partition_locked(no_regather_partition_ids, partition_stat_infos)) {
        if (OB_FAIL(append(stat_table.no_regather_partition_ids_, no_regather_partition_ids))) {
          LOG_WARN("failed to append table ids");
        } else {
          stat_table.stale_percent_ = -1.0;
          stat_table.incremental_stat_ = true;
          stat_table.need_gather_subpart_ = PARTITION_LEVEL_TWO == table_schema.get_part_level();
        }
      } else {//no stale partition
        stat_table.stale_percent_ = 0.0;
        stat_table.incremental_stat_ = false;
      }
    }
  }
  LOG_TRACE("succeed to get user partition table stale percent",
              K(stat_table), K(partition_stat_infos), K(is_big_table));
  return ret;
}

int ObDbmsStats::gather_tables_stats_with_default_param(ObExecContext &ctx,
                                                        ObGatherTableStatsHelper &helper,
                                                        ObOptStatTaskInfo &task_info)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < helper.stat_tables_.count(); ++i) {
    if (is_oceanbase_sys_database_id(helper.stat_tables_.at(i).stat_table_.database_id_)) {
      lib::CompatModeGuard compat_guard(lib::Worker::CompatMode::MYSQL);
      if (OB_FAIL(gather_table_stats_with_default_param(ctx,
                                                        helper.duration_time_,
                                                        helper.stat_tables_.at(i).stat_table_,
                                                        task_info))) {
        LOG_WARN("failed to gather table stats with default param", K(ret));
      }
    } else if (OB_FAIL(gather_table_stats_with_default_param(ctx,
                                                             helper.duration_time_,
                                                             helper.stat_tables_.at(i).stat_table_,
                                                             task_info))) {
      LOG_WARN("failed to gather table stats with default param", K(ret));
    }
    if (OB_FAIL(ret)) {
      if (OB_ERR_QUERY_INTERRUPTED == ret) {
        LOG_WARN("query interrupted", K(ret));
      } else if (OB_TABLE_NOT_EXIST == ret || OB_TIMEOUT == ret) {
        // do nothing
        ++helper.failed_count_;
        ret = OB_SUCCESS;
      } else {
        ++helper.failed_count_;
        LOG_WARN("failed to gather table stats with some unknown reason", K(ret));
        ret = OB_SUCCESS;
      }
    } else {
      ++helper.succeed_count_;
    }
  }
  return ret;
}

int ObDbmsStats::gather_table_stats_with_default_param(ObExecContext &ctx,
                                                       const int64_t duration_time,
                                                       const StatTable &stat_table,
                                                       ObOptStatTaskInfo &task_info)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc("OptStatGather", OB_MALLOC_NORMAL_BLOCK_SIZE, ctx.get_my_session()->get_effective_tenant_id());
  ObTableStatParam stat_param;
  stat_param.allocator_ = &tmp_alloc;
  stat_param.db_id_ = stat_table.database_id_;
  bool is_all_fast_gather = false;
  ObSEArray<int64_t, 4> no_gather_index_ids;
  ObOptStatGatherStat gather_stat(task_info);
  gather_stat.set_table_id(stat_table.table_id_);
  ObOptStatGatherStatList::instance().push(gather_stat);
  ObOptStatRunningMonitor running_monitor(ctx.get_allocator(), ObTimeUtility::current_time(), stat_param.allocator_->used(), gather_stat);
  if (OB_FAIL(ObDbmsStatsUtils::get_valid_duration_time(task_info.task_start_time_,
                                                        duration_time,
                                                        stat_param.duration_time_))) {
    LOG_WARN("failed to get valid duration time", K(ret));
  } else if (OB_FAIL(parse_table_part_info(ctx, stat_table, stat_param))) {
    LOG_WARN("failed to parse owner", K(ret));
  } else if (OB_FAIL(use_default_gather_stat_options(ctx, stat_table, stat_param))) {
    LOG_WARN("failed to use default gather stat optitions", K(ret));
  } else if (stat_table.need_gather_subpart_) {
    stat_param.subpart_stat_param_.set_gather_stat();
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(running_monitor.add_table_info(stat_param, stat_table.stale_percent_))) {
      LOG_WARN("failed to add table info", K(ret));
    } else if (OB_FAIL(ObDbmsStatsLockUnlock::adjust_table_stat_param(stat_table.no_regather_partition_ids_,
                                                                      stat_param))) {// Note:
      LOG_WARN("failed to adjust table stat param", K(ret));
    } else if (OB_FAIL(ObDbmsStatsExecutor::gather_table_stats(ctx, stat_param))) {// Note:
      LOG_WARN("failed to gather table stats", K(ret));
    } else if (OB_FAIL(update_stat_cache(ctx.get_my_session()->get_rpc_tenant_id(),
                                         stat_param,
                                         &running_monitor))) {
      LOG_WARN("failed to update stat cache", K(ret));
    //refresh duration time
    } else if (OB_FAIL(ObDbmsStatsUtils::get_valid_duration_time(task_info.task_start_time_,
                                                                 duration_time,
                                                                 stat_param.duration_time_))) {
      LOG_WARN("failed to get valid duration time", K(ret));
    } else if (!need_gather_index_stats(stat_param)) {
      LOG_TRACE("Succeed to gather table stats", K(stat_param));
    } else if (stat_param.cascade_ &&
               OB_FAIL(fast_gather_index_stats(ctx, stat_param,
                                               is_all_fast_gather, no_gather_index_ids))) {
      LOG_WARN("failed to fast gather index stats", K(ret));
    //refresh duration time
    } else if (OB_FAIL(ObDbmsStatsUtils::get_valid_duration_time(task_info.task_start_time_,
                                                                 duration_time,
                                                                 stat_param.duration_time_))) {
      LOG_WARN("failed to get valid duration time", K(ret));
    } else if (stat_param.cascade_ && !is_all_fast_gather &&
               OB_FAIL(gather_table_index_stats(ctx, stat_param, no_gather_index_ids))) {
      LOG_WARN("failed to gather table index stats", K(ret));
    } else {
      LOG_TRACE("Succeed to gather table stats", K(stat_param));
    }
  }
  running_monitor.set_monitor_result(ret, ObTimeUtility::current_time(), stat_param.allocator_->used());
  ObOptStatManager::get_instance().update_opt_stat_gather_stat(gather_stat);
  ObOptStatGatherStatList::instance().remove(gather_stat);
  task_info.completed_table_count_ ++;
  return ret;
}

/* @brief, ObDbmsStatsPreferences::check_prefs_validity, check common prefs for user prefs and
 *   global prefs, now only this following prefs is valid for OceanBase, So we just support following
 *   prefs:
 *     CASCADE, DEGREE, ESTIMATE_PERCENT, GRANULARITY, INCREMENTAL, INCREMENTAL_LEVEL,
 *     METHOD_OPT, NO_INVALIDATE, OPTIONS, STALE_PERCENT, APPROXIMATE_NDV(global prefs unique),
 *   The following prefs not used in OceanBase now, maybe used in the future:
 *     common prefs:
 *     INCREMENTAL_STALENESS, PUBLISH, TABLE_CACHED_BLOCKS
 *     global unique prefs:
 *     ANDV_ALGO_INTERNAL_OBSERVE, APPROXIMATE_NDV_ALGORITHM, AUTOSTATS_TARGET,AUTO_STAT_EXTENSIONS,
 *     CONCURRENT, DEBUG, ENABLE_HYBRID_HISTOGRAMS, INCREMENTAL_INTERNAL_CONTROL, JOB_OVERHEAD,
 *     JOB_OVERHEAD_PERC, PREFERENCE_OVERRIDES_PARAMETER, SCAN_RATE, STAT_CATEGORY, SYS_FLAGS,
 *     TRACE, WAIT_TIME_TO_UPDATE_STATS
 *  add new prefs for OceanBase: ESTIMATE_BLOCK
   https://docs.oracle.com/database/121/ARPLS/d_stats.htm#ARPLS68674
*/
int ObDbmsStats::get_new_stat_pref(ObExecContext &ctx,
                                   common::ObIAllocator &allocator,
                                   ObString &opt_name,
                                   ObString &opt_value,
                                   bool is_global_prefs,
                                   ObStatPrefs *&stat_pref)
{
  int ret = OB_SUCCESS;
  ObCharset::caseup(ctx.get_my_session()->get_local_collation_connection(), opt_name);
  ObCharset::caseup(ctx.get_my_session()->get_local_collation_connection(), opt_value);
  if (0 == opt_name.case_compare("CASCADE")) {
    ObCascadePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("DEGREE")) {
    ObDegreePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("ESTIMATE_PERCENT")) {
    ObEstimatePercentPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("GRANULARITY")) {
    ObGranularityPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("INCREMENTAL")) {
    ObIncrementalPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("INCREMENTAL_LEVEL")) {
    ObIncrementalLevelPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("METHOD_OPT")) {
    ObMethodOptPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("NO_INVALIDATE")) {
    ObNoInvalidatePrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("OPTIONS")) {
    ObOptionsPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("STALE_PERCENT")) {
    ObStalePercentPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (is_global_prefs && 0 == opt_name.case_compare("APPROXIMATE_NDV")) {
    ObApproximateNdvPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else if (0 == opt_name.case_compare("ESTIMATE_BLOCK")) {
    ObEstimateBlockPrefs *tmp_pref = NULL;
    if (OB_FAIL(new_stat_prefs(allocator, ctx.get_my_session(), opt_value, tmp_pref))) {
      LOG_WARN("failed to new stat prefs", K(ret));
    } else {
      stat_pref = tmp_pref;
    }
  } else {
    ret = OB_ERR_DBMS_STATS_PL;
    LOG_WARN("Invalid input values for pname", K(ret), K(opt_name));
    LOG_USER_ERROR(OB_ERR_DBMS_STATS_PL, "Invalid input values for pname, Only Support CASCADE |"\
                                       "DEGREE | ESTIMATE_PERCENT | GRANULARITY | INCREMENTAL |"\
                                       "INCREMENTAL_LEVEL | METHOD_OPT | NO_INVALIDATE | OPTIONS"\
                                      "STALE_PERCENT | ESTIMATE_BLOCK | APPROXIMATE_NDV(global prefs unique) prefs");
  }
  return ret;
}

int ObDbmsStats::get_table_stale_percent_threshold(sql::ObExecContext &ctx,
                                                   const uint64_t tenant_id,
                                                   const uint64_t table_id,
                                                   double &stale_percent_threshold)
{
  int ret = OB_SUCCESS;
  ObObj result;
  ObTableStatParam param;
  ObString opt_name("STALE_PERCENT");
  ObArenaAllocator tmp_alloc("OptStatPrefs", OB_MALLOC_NORMAL_BLOCK_SIZE, tenant_id);
  param.tenant_id_ = tenant_id;
  param.table_id_ = table_id;
  param.allocator_ = &tmp_alloc;
  if (OB_FAIL(ObDbmsStatsPreferences::get_prefs(ctx, param, opt_name, result))) {
    LOG_WARN("failed to get prefs", K(ret));
  } else if (!result.is_null()) {
    ObArenaAllocator calc_buf(ObModIds::OB_SQL_PARSER);
    ObCastCtx cast_ctx(&calc_buf, NULL, CM_NONE, ObCharset::get_system_collation());
    ObObj dest_obj;
    if (OB_FAIL(ObObjCaster::to_type(ObDoubleType, cast_ctx, result, dest_obj))) {
      LOG_WARN("failed to cast number to double type", K(ret));
    } else if (OB_FAIL(dest_obj.get_double(stale_percent_threshold))) {
      LOG_WARN("failed to get double", K(ret));
    } else {
      stale_percent_threshold = stale_percent_threshold / 100.0;
      LOG_TRACE("Succeed to get table stale percent threshold", K(stale_percent_threshold));
    }
  }
  return ret;
}

int ObDbmsStats::convert_vaild_ident_name(common::ObIAllocator &allocator,
                                          const common::ObDataTypeCastParams &dtc_params,
                                          ObString &ident_name,
                                          bool need_extra_conv/*default false*/)
{
  int ret = OB_SUCCESS;
  if (!ident_name.empty()) {
    if (OB_FAIL(ObSQLUtils::convert_sql_text_to_schema_for_storing(allocator,
                                                                   dtc_params,
                                                                   ident_name))) {
      LOG_WARN("fail to convert charset", K(ret));
    } else if (need_extra_conv) {
      //oracle support lowercase name to gather and manager stats, eg:
      //  create table "t1"(c1 int);
      //  call dbms_stats.gather_table_stats(NULL, '"t1"');
      if (ident_name.length() > 1 &&
          ident_name.ptr()[0] == '\"' &&
          ident_name.ptr()[ident_name.length() - 1] == '\"') {
        ident_name.assign(ident_name.ptr() + 1, ident_name.length() - 2);
      } else {
        ObCharset::caseup(CS_TYPE_UTF8MB4_BIN, ident_name);
      }
    }
  }
  return ret;
}

bool ObDbmsStats::is_table_gather_global_stats(const int64_t global_id,
                                               const ObIArray<ObPartitionStatInfo> &partition_stat_infos,
                                               int64_t &cur_row_cnt)
{
  bool is_gather = false;
  cur_row_cnt = 0;
  for (int64_t i = 0; !is_gather && i < partition_stat_infos.count(); ++i) {
    if (global_id == partition_stat_infos.at(i).partition_id_) {
      is_gather = true;
      cur_row_cnt = partition_stat_infos.at(i).row_cnt_;
    }
  }
  return is_gather;
}

bool ObDbmsStats::is_all_partition_locked(const ObIArray<int64_t> &partition_ids,
                                          const ObIArray<ObPartitionStatInfo> &partition_stat_infos)
{
  bool is_all_locked = !partition_ids.empty();
  for (int64_t i = 0; is_all_locked && i < partition_ids.count(); ++i) {
    is_all_locked = !partition_stat_infos.empty();
    bool find_it = false;
    for (int64_t j = 0; is_all_locked && !find_it && j < partition_stat_infos.count(); ++j) {
      if (partition_ids.at(i) == partition_stat_infos.at(j).partition_id_) {
        is_all_locked = partition_stat_infos.at(j).is_stat_locked_;
        find_it = true;
      } else {/*do nothing*/}
    }
    if (!find_it) {//not found
      is_all_locked = false;
    }
  }
  return is_all_locked;
}

int ObDbmsStats::get_table_index_infos(sql::ObExecContext &ctx,
                                       const int64_t table_id,
                                       ObIArray<ObAuxTableMetaInfo> &index_infos)
{
  int ret = OB_SUCCESS;
  const share::schema::ObTableSchema *table_schema = NULL;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  if (OB_ISNULL(schema_guard)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(schema_guard));
  } else if (OB_FAIL(schema_guard->get_table_schema(
                     ctx.get_my_session()->get_effective_tenant_id(),
                     table_id,
                     table_schema))) {
    LOG_WARN("failed to get table schema", K(ret));
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret), K(table_schema));
  } else if (share::is_oracle_mapping_real_virtual_table(table_schema->get_table_id())) {
    // do not gather stat for oracle inner table index
  } else if (OB_FAIL(table_schema->get_simple_index_infos(index_infos, false))) {
    LOG_WARN("failed to get simple index infos", K(ret));
  } else {
    LOG_TRACE("Succeed to get table index infos", K(table_id), K(index_infos));
  }
  return ret;
}

int ObDbmsStats::get_index_schema(sql::ObExecContext &ctx,
                                  common::ObIAllocator &allocator,
                                  const int64_t data_table_id,
                                  const bool is_sensitive_compare,
                                  ObString &index_name,
                                  const share::schema::ObTableSchema *&index_schema)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard *schema_guard = ctx.get_virtual_table_ctx().schema_guard_;
  index_schema = NULL;
  ObSEArray<ObAuxTableMetaInfo, 4> simple_index_infos;
  if (OB_ISNULL(schema_guard)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected null", K(ret));
  } else if (OB_FAIL(get_table_index_infos(ctx, data_table_id, simple_index_infos))) {
    LOG_WARN("failed to get table index infos", K(ret));
  } else {
    bool found_it = false;
    for (int64_t i = 0; OB_SUCC(ret) && !found_it && i < simple_index_infos.count(); ++i) {
      const share::schema::ObTableSchema *cur_index_schema = NULL;
      ObString cur_index_name;
      if (simple_index_infos.at(i).table_id_ == data_table_id) {
        //do nothing, remove primary table
      } else if (OB_FAIL(schema_guard->get_table_schema(
                 ctx.get_my_session()->get_effective_tenant_id(),
                 simple_index_infos.at(i).table_id_, cur_index_schema))) {
        LOG_WARN("failed to get table schema", K(ret));
      } else if (OB_ISNULL(cur_index_schema) || OB_UNLIKELY(!cur_index_schema->is_index_table())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("get unexpected null", K(ret), KPC(cur_index_schema));
      } else if (OB_FAIL(cur_index_schema->get_index_name(cur_index_name))) {
        LOG_WARN("failed to get index name", K(ret));
      } else if ((is_sensitive_compare &&
                  ObCharset::case_sensitive_equal(cur_index_name, index_name)) ||
                 (!is_sensitive_compare &&
                  ObCharset::case_insensitive_equal(cur_index_name, index_name))) {
        if (OB_FAIL(ob_write_string(allocator, cur_index_name, index_name))) {
          LOG_WARN("failed to write string", K(ret));
        } else {
          found_it = true;
          index_schema = cur_index_schema;
        }
      } else {
        LOG_TRACE("index schema isn't fullfill with the specified index name", K(cur_index_name),
                                                                               K(index_name));
      }
    }
    if (!found_it) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("index schema is null", K(ret), K(index_schema), K(index_name));
      LOG_USER_ERROR(OB_TABLE_NOT_EXIST, to_cstring(index_name), to_cstring(index_name));
    }
  }
  return ret;
}

int ObDbmsStats::set_param_global_part_id(ObExecContext &ctx,
                                          ObTableStatParam &param, 
                                          bool is_data_table,
                                          int64_t data_table_id,
                                          share::schema::ObPartitionLevel data_table_level)
{
  int ret = OB_SUCCESS;
  share::schema::ObPartitionLevel part_level = is_data_table ? data_table_level : param.part_level_;
  int64_t target_table_id = is_data_table ? data_table_id : param.table_id_;
  if (part_level == share::schema::ObPartitionLevel::PARTITION_LEVEL_ZERO) {
    ObDASTabletMapper tablet_mapper;
    ObSEArray<ObTabletID, 1> tmp_tablet_ids;
    ObSEArray<ObObjectID, 1> tmp_part_ids;
    if (OB_FAIL(ctx.get_das_ctx().get_das_tablet_mapper(target_table_id, tablet_mapper))) {
      LOG_WARN("fail to get das tablet mapper", K(ret));
    } else if (OB_FAIL(tablet_mapper.get_non_partition_tablet_id(tmp_tablet_ids, tmp_part_ids))) {
      LOG_WARN("failed to get non partition tablet id", K(ret));
    } else if (tmp_part_ids.count() == 1 && tmp_tablet_ids.count() == 1) {
      if (is_data_table) {
        // if the table is the data table for index, only need
        param.global_data_part_id_ = static_cast<int64_t>(tmp_part_ids.at(0));
      } else {
        param.global_part_id_ = is_virtual_table(target_table_id) ?
                                         target_table_id : static_cast<int64_t>(tmp_part_ids.at(0));
        param.global_tablet_id_ = tmp_tablet_ids.at(0).id();
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected error", K(ret), K(param), K(tmp_tablet_ids), K(tmp_part_ids));
    }
    if (OB_SUCC(ret) && is_data_table && (!param.is_index_param())) {
        param.global_data_part_id_ = ObTableStatParam::DEFAULT_DATA_PART_ID;
    }
  }
  return ret;
}

int ObDbmsStats::get_table_partition_infos(const ObTableSchema &table_schema,
                                           ObIArray<PartInfo> &partition_infos)
{
  int ret = OB_SUCCESS;
  ObSEArray<PartInfo, 4> part_infos;
  ObSEArray<PartInfo, 4> subpart_infos;
  ObSEArray<int64_t, 4> part_ids;
  ObSEArray<int64_t, 4> subpart_ids;
  if (OB_FAIL(get_table_part_infos(&table_schema,
                                   part_infos,
                                   subpart_infos,
                                   part_ids,
                                   subpart_ids))) {
    LOG_WARN("failed to get table part infos", K(ret));
  } else if (PARTITION_LEVEL_ONE == table_schema.get_part_level() &&
             OB_FAIL(partition_infos.assign(part_infos))) {
    LOG_WARN("failed to assign", K(ret));
  } else if (PARTITION_LEVEL_TWO == table_schema.get_part_level() &&
             OB_FAIL(partition_infos.assign(subpart_infos))) {
    LOG_WARN("failed to assign", K(ret));
  } else {/*do nothing*/}
  return ret;
}

int ObDbmsStats::get_table_partition_map(const ObTableSchema &table_schema,
                                         OSGPartMap &part_map)
{
  int ret = OB_SUCCESS;
  if (PARTITION_LEVEL_TWO != table_schema.get_part_level()
      && PARTITION_LEVEL_ONE != table_schema.get_part_level()) {
  } else {
    ObSEArray<PartInfo, 4> part_infos;
    ObSEArray<PartInfo, 4> subpart_infos;
    ObSEArray<int64_t, 4> part_ids;
    ObSEArray<int64_t, 4> subpart_ids;
    if (OB_FAIL(get_table_part_infos(&table_schema,
                                    part_infos,
                                    subpart_infos,
                                    part_ids,
                                    subpart_ids,
                                    &part_map))) {
      LOG_WARN("failed to get table part infos", K(ret));
    }
  }
  return ret;
}

bool ObDbmsStats::is_func_index(const ObTableStatParam &index_param)
{
  bool is_true = false;
  for (int64_t i = 0; !is_true && i < index_param.column_params_.count(); ++i) {
    is_true = index_param.column_params_.at(i).is_hidden_column();
  }
  return is_true;
}

bool ObDbmsStats::need_gather_index_stats(const ObTableStatParam &param)
{
  return !(is_virtual_table(param.table_id_) ||
           share::schema::ObTableType::EXTERNAL_TABLE == param.ref_table_type_);
}

/**
 * @brief ObDbmsStats::parse_granularity
 * @param ctx
 * @param granularity
 * possible values are:
 *  ALL: Gather all (subpartition, partition, and global)
 *  AUTO: Oracle recommends setting granularity to the default value of AUTO to gather subpartition,
 *        partition, or global statistics, depending on partition type.
 *  DEFAULT: Gathers global and partition-level
 *  GLOBAL: Gather global only
 *  GLOBAL AND PARTITION: Gather global and partition-level
 *  APPROX_GLOBAL AND PARTITION: similar to 'GLOBAL AND PARTITION' but in this case the global
                                 statistics are aggregated from partition level statistics.
 *  PARTITION: Gather partition-level
 *  SUBPARTITION: Gather subpartition-level
 *  Oracle granularity actual behavior survey:
 *
 * @return
 */
int ObDbmsStats::resovle_granularity(ObGranularityType granu_type,
                                     const bool use_size_auto,
                                     ObTableStatParam &param)
{
  int ret = OB_SUCCESS;
  if (ObGranularityType::GRANULARITY_AUTO == granu_type) {
    param.global_stat_param_.set_gather_stat(false);
    param.part_stat_param_.set_gather_stat();
    param.subpart_stat_param_.set_gather_stat();
    // refine auto granularity based on subpart type
    if (ObPartitionLevel::PARTITION_LEVEL_TWO == param.part_level_ &&
        !(is_range_part(param.subpart_stat_param_.part_type_) || is_list_part(param.subpart_stat_param_.part_type_))) {
      param.subpart_stat_param_.gather_histogram_ = !use_size_auto;
    }
  } else if (ObGranularityType::GRANULARITY_ALL == granu_type) {
    param.global_stat_param_.set_gather_stat(false);
    param.part_stat_param_.set_gather_stat();
    param.subpart_stat_param_.set_gather_stat();
  } else if (ObGranularityType::GRANULARITY_GLOBAL_AND_PARTITION == granu_type) {
    param.global_stat_param_.set_gather_stat(false);
    param.part_stat_param_.set_gather_stat();
    param.subpart_stat_param_.reset_gather_stat();
  } else if (ObGranularityType::GRANULARITY_APPROX_GLOBAL_AND_PARTITION == granu_type) {
    bool gather_approx = param.part_level_ != ObPartitionLevel::PARTITION_LEVEL_ZERO;
    param.global_stat_param_.set_gather_stat(gather_approx);
    param.part_stat_param_.set_gather_stat();
    param.subpart_stat_param_.reset_gather_stat();
  } else if (ObGranularityType::GRANULARITY_GLOBAL == granu_type) {
    param.global_stat_param_.set_gather_stat(false);
    param.part_stat_param_.reset_gather_stat();
    param.subpart_stat_param_.reset_gather_stat();
  } else if (ObGranularityType::GRANULARITY_PARTITION == granu_type) {
    param.global_stat_param_.reset_gather_stat();
    param.part_stat_param_.set_gather_stat();
    param.subpart_stat_param_.reset_gather_stat();
  } else if (ObGranularityType::GRANULARITY_SUBPARTITION == granu_type) {
    param.global_stat_param_.reset_gather_stat();
    param.part_stat_param_.reset_gather_stat();
    param.subpart_stat_param_.set_gather_stat();
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("get unexpected granularity type", K(granu_type));
  }
  LOG_TRACE("succeed to parse granularity", K(param.global_stat_param_),
              K(param.part_stat_param_), K(param.subpart_stat_param_));
  return ret;
}

void ObDbmsStats::decide_modified_part(ObTableStatParam &param, const bool cascade_parts)
{
  if (param.part_name_.empty()) {
    param.global_stat_param_.need_modify_ = true;
    param.part_stat_param_.need_modify_ = cascade_parts;
    param.subpart_stat_param_.need_modify_ = cascade_parts;
  } else if (!param.is_subpart_name_) {
    param.global_stat_param_.need_modify_ = false;
    param.part_stat_param_.need_modify_ = true;
    param.subpart_stat_param_.need_modify_ = cascade_parts;
  } else {
    param.global_stat_param_.need_modify_ = false;
    param.part_stat_param_.need_modify_ = false;
    param.subpart_stat_param_.need_modify_ = true;
  }
  if (!param.part_stat_param_.need_modify_) {
    param.part_infos_.reset();
  }
  if (!param.subpart_stat_param_.need_modify_) {
    param.subpart_infos_.reset();
  }
}

int ObDbmsStats::init_gather_task_info(ObExecContext &ctx,
                                       ObOptStatGatherType type,
                                       int64_t start_time,
                                       int64_t task_table_count,
                                       ObOptStatTaskInfo &task_info)
{
  int ret = OB_SUCCESS;
  ObString task_id;
  char *server_uuid = NULL;
  int64_t length_uuid = 36;
  if (OB_ISNULL(server_uuid = static_cast<char*>(ctx.get_allocator().alloc(length_uuid)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("memory is not enough", K(ret), K(length_uuid));
  } else if (OB_FAIL(ObExprUuid::gen_server_uuid(server_uuid, length_uuid))) {
    LOG_WARN("failed to gen server uuid", K(ret));
  } else {
    task_id.assign_ptr(server_uuid, length_uuid);
    if (OB_ISNULL(ctx.get_my_session())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("get unexpected null", K(ret), K(ctx.get_my_session()));
    } else if (OB_FAIL(task_info.init(ctx.get_allocator(),
                                      ctx.get_my_session()->get_effective_tenant_id(),
                                      ctx.get_my_session()->get_sessid(),
                                      ctx.get_my_session()->get_current_trace_id(),
                                      task_id,
                                      type,
                                      start_time,
                                      task_table_count))) {
      LOG_WARN("failed to init", K(ret));
    } else {
      LOG_TRACE("Succeed to init gather task info", K(task_info));
    }
  }
  return ret;
}

}
}
