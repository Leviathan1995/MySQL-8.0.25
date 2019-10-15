var common_stmts = require("common_statements");

var options = {
  cluster_type: "gr",
  innodb_cluster_name: "mycluster",
  innodb_cluster_instances: [ ["localhost", 5500], ["localhost", 5510], ["localhost", 5520] ],
};

var common_responses = common_stmts.prepare_statement_responses([
  "router_select_cluster_type_v2",
  "router_select_group_membership_with_primary_mode",
  "router_select_group_replication_primary_member",
  "router_select_metadata_v2",
  "router_count_clusters_v2",
  "router_check_member_state",
  "router_select_members_count",
  "router_select_replication_group_name",
  "router_show_cipher_status",
  "router_select_cluster_instances_v2",
  "router_select_cluster_instance_addresses_v2",
  "router_start_transaction",
  "router_commit",
], options);

var router_select_schema_version = common_stmts.get("router_select_schema_version", options);

var common_responses_regex = common_stmts.prepare_statement_responses_regex([
  "router_insert_into_routers",
  "router_create_user_if_not_exists",
  "router_grant_on_metadata_db",
  "router_grant_on_pfs_db",
  "router_grant_on_routers",
  "router_grant_on_v2_routers",
  "router_update_routers_in_metadata",
], options);

({
  stmts: function (stmt) {
    var res;
    if (common_responses.hasOwnProperty(stmt)) {
      return common_responses[stmt];
    }
    if (stmt === router_select_schema_version.stmt) {
      router_select_schema_version.exec_time = 2000.0;
      return router_select_schema_version;
    }
    else if ((res = common_stmts.handle_regex_stmt(stmt, common_responses_regex)) !== undefined) {
      return res;
    }
    else {
      return common_stmts.unknown_statement_response(stmt);
    }
  }
})