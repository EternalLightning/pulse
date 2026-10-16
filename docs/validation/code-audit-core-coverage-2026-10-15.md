# Core 逐文件覆盖（2026-10-15 续审）

仅记录当前续审直接阅读。旧会话未留下逐文件全文证据的文件不补记为全文；未读不是确认无问题。

2026-10-16 本轮补齐此前 49 项全文记录，清单共 157 项均有全文阅读证据。全文覆盖只表示阅读完成；未修复发现、故障注入和实际 UI/服务验证仍须按各报告追踪，不表示全仓审查结束。
| 文件 | 本次状态 |
|---|---|
| src/common/command_line.h | 全文 |
| src/common/crash_reporter.cpp | 2026-10-16 全文，见 code-audit-shell-crash-full-2026-10-16.md |
| src/common/crash_reporter.h | 2026-10-16 全文，见 code-audit-shell-crash-full-2026-10-16.md |
| src/common/current_user_security.h | 全文 |
| src/common/diagnostics_exporter.cpp | 2026-10-16 全文，见 code-audit-drag-drop-2026-10-16.md |
| src/common/diagnostics_exporter.h | 2026-10-16 全文，见 code-audit-drag-drop-2026-10-16.md |
| src/common/display_path.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/file_create.h | 全文 |
| src/common/file_identity.h | 全文 |
| src/common/file_operation_error.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/json_utils.h | 全文 |
| src/common/known_folder_labels.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/localization.cpp | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/localization.h | 2026-10-16 全文，见 code-audit-resources-full-2026-10-16.md |
| src/common/path_utils.h | 全文 |
| src/common/preview_extensions.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/progress_percent.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/resource_ids.h | 2026-10-16 全文，见 code-audit-resources-full-2026-10-16.md |
| src/common/runtime_messages.inc | 2026-10-16 全文，见 code-audit-resources-full-2026-10-16.md |
| src/common/shell_menu_item.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/string_ids.h | 2026-10-16 全文，见 code-audit-resources-full-2026-10-16.md |
| src/common/strings.rcinc | 2026-10-16 全文，见 code-audit-resources-full-2026-10-16.md |
| src/common/text_decode.cpp | 全文 |
| src/common/text_decode.h | 全文 |
| src/common/text_format.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/common/unique_resource.h | 全文 |
| src/common/user_storage.cpp | 2026-10-16 全文，见 code-audit-drag-drop-2026-10-16.md |
| src/common/user_storage.h | 2026-10-16 全文，见 code-audit-drag-drop-2026-10-16.md |
| src/common/utf8_file.h | 全文 |
| src/common/windows_compat.h | 2026-10-16 全文，见 code-audit-small-common-2026-10-16.md |
| src/index/change_feed_history.h | 全文 |
| src/index/change_journal_writer.h | 2026-10-16 全文，见 code-audit-small-index-boundaries-2026-10-16.md |
| src/index/change_summary_accumulator.h | 全文 |
| src/index/change_tracking_client.cpp | 2026-10-16 全文，见 code-audit-change-tracking-full-2026-10-16.md |
| src/index/change_tracking_client.h | 2026-10-16 全文，见 code-audit-change-tracking-full-2026-10-16.md |
| src/index/change_tracking.cpp | 2026-10-16 全文，见 code-audit-change-tracking-full-2026-10-16.md |
| src/index/change_tracking.h | 2026-10-16 全文，见 code-audit-change-tracking-full-2026-10-16.md |
| src/index/content_agent.cpp | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_agent.h | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_config_storage.h | 全文 |
| src/index/content_configuration.cpp | 2026-10-16 全文，见 code-audit-small-configuration-2026-10-16.md |
| src/index/content_hit_aliases.cpp | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_hit_aliases.h | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_index.cpp | 2026-10-16 全文，见 code-audit-content-index-full-2026-10-16.md |
| src/index/content_index.h | 2026-10-16 全文，见 code-audit-small-configuration-2026-10-16.md |
| src/index/content_instant_session.cpp | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_instant_session.h | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_result_store.cpp | 全文 |
| src/index/content_result_store.h | 全文 |
| src/index/content_scope.h | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_search_client.cpp | 2026-10-16 全文，见 code-audit-content-client-lifecycle-2026-10-16.md |
| src/index/content_search_client.h | 2026-10-16 全文，见 code-audit-content-client-lifecycle-2026-10-16.md |
| src/index/content_search_protocol.h | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_search_session.cpp | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_search_session.h | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_search.cpp | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_search.h | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_sort_sql.h | 2026-10-16 全文，见 code-audit-small-content-2026-10-16.md |
| src/index/content_task_search.cpp | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_task_search.h | 2026-10-16 全文，见 code-audit-content-full-2026-10-16.md |
| src/index/content_timing.cpp | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/content_timing.h | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/document_admission.h | 全文 |
| src/index/document_file_version.h | 全文 |
| src/index/document_filter.cpp | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/document_filter.h | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/document_host_main.cpp | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/document_literal_match.h | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/document_memory_trace.h | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/document_metrics.h | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/document_protocol.h | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/document_reader.cpp | 2026-10-16 全文，见 code-audit-document-full-2026-10-16.md |
| src/index/document_reader.h | 2026-10-16 全文，见 code-audit-document-full-2026-10-16.md |
| src/index/filename_pinyin.cpp | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/filename_pinyin.h | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/filename_timing.cpp | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/filename_timing.h | 2026-10-16 全文，见 code-audit-small-timing-2026-10-16.md |
| src/index/folder_size_index.h | 2026-10-16 全文，见 code-audit-small-folder-size-2026-10-16.md |
| src/index/folder_size_protocol.h | 2026-10-16 全文，见 code-audit-small-folder-size-2026-10-16.md |
| src/index/index_caller_visibility.h | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/index_client.cpp | 2026-10-16 全文，见 code-audit-index-host-full-2026-10-16.md |
| src/index/index_client.h | 2026-10-16 全文，见 code-audit-index-host-full-2026-10-16.md |
| src/index/index_config.cpp | 2026-10-16 全文，见 code-audit-small-configuration-2026-10-16.md |
| src/index/index_config.h | 2026-10-16 全文，见 code-audit-small-configuration-2026-10-16.md |
| src/index/index_delta.cpp | 全文 |
| src/index/index_delta.h | 全文 |
| src/index/index_diagnostics.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_engine_pinyin.cpp | 2026-10-16 全文，见 code-audit-query-full-2026-10-16.md |
| src/index/index_engine.cpp | 2026-10-16 全文，见 code-audit-engine-full-2026-10-16.md |
| src/index/index_engine.h | 2026-10-16 全文，见 code-audit-engine-full-2026-10-16.md |
| src/index/index_feed.h | 2026-10-16 全文，见 code-audit-change-tracking-full-2026-10-16.md |
| src/index/index_folder_sizes.cpp | 2026-10-16 全文，见 code-audit-small-folder-size-2026-10-16.md |
| src/index/index_host.cpp | 2026-10-16 全文，见 code-audit-index-host-full-2026-10-16.md |
| src/index/index_host.manifest | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_host.rc | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_maintenance_probe.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_memory_probe.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_mft.cpp | 2026-10-16 全文，见 code-audit-index-host-full-2026-10-16.md |
| src/index/index_mft.h | 2026-10-16 全文，见 code-audit-index-host-full-2026-10-16.md |
| src/index/index_migration.cpp | 全文 |
| src/index/index_migration.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_name_pool.h | 全文 |
| src/index/index_parent_chain.h | 全文 |
| src/index/index_path_service.cpp | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/index_path_service.h | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/index_paths.cpp | 全文 |
| src/index/index_paths.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/index_protocol.h | 2026-10-16 全文，见 code-audit-small-index-boundaries-2026-10-16.md |
| src/index/index_query.cpp | 2026-10-16 全文，见 code-audit-query-full-2026-10-16.md |
| src/index/index_query.h | 2026-10-16 全文，见 code-audit-query-full-2026-10-16.md |
| src/index/index_raw_storage_security.h | 全文 |
| src/index/index_request_limits.h | 全文 |
| src/index/index_search_response.h | 全文 |
| src/index/index_server_auth.h | 全文 |
| src/index/index_service_start.h | 2026-10-16 全文，见 code-audit-small-index-boundaries-2026-10-16.md |
| src/index/index_shard.cpp | 全文 |
| src/index/index_shard.h | 全文 |
| src/index/index_transport_security.h | 全文 |
| src/index/index_visibility.cpp | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/live_filename_search.h | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_agent_client.cpp | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_agent_client.h | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_agent_host.h | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_agent_main.cpp | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_agent_protocol.h | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_index.cpp | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/network_index.h | 2026-10-16 全文，见 code-audit-network-full-2026-10-16.md |
| src/index/office_ooxml.cpp | 2026-10-16 全文，见 code-audit-document-full-2026-10-16.md |
| src/index/office_ooxml.h | 2026-10-16 全文，见 code-audit-document-full-2026-10-16.md |
| src/index/pdfium_text.cpp | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/pdfium_text.h | 2026-10-16 全文，见 code-audit-small-document-2026-10-16.md |
| src/index/search_kinds.cpp | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/search_kinds.h | 2026-10-16 全文，见 code-audit-small-index-helpers-2026-10-16.md |
| src/index/search_result_merge.cpp | 2026-10-16 全文，见 code-audit-query-full-2026-10-16.md |
| src/index/search_result_merge.h | 2026-10-16 全文，见 code-audit-query-full-2026-10-16.md |
| src/index/search_trace.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/text_task_reader.cpp | 2026-10-16 全文，见 code-audit-small-text-reader-2026-10-16.md |
| src/index/text_task_reader.h | 2026-10-16 全文，见 code-audit-small-text-reader-2026-10-16.md |
| src/index/user_index_storage.cpp | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/user_index_storage.h | 2026-10-16 全文，见 code-audit-small-storage-probes-2026-10-16.md |
| src/index/usn_packet_queue.h | 全文 |
| src/index/usn_read_wait.h | 全文 |
| src/index/usn_stream.h | 2026-10-16 全文，见 code-audit-small-index-boundaries-2026-10-16.md |
| src/ipc/ctx_menu_util.h | 2026-10-16 全文，见 code-audit-shell-crash-full-2026-10-16.md |
| src/ipc/delete_plan_protocol.h | 全文 |
| src/ipc/file_identity_protocol.h | 全文 |
| src/ipc/preview_protocol.h | 全文 |
| src/ipc/preview_response.h | 全文 |
| src/ipc/protocol.h | 全文 |
| src/ipc/shell_client.cpp | 2026-10-16 全文，见 code-audit-shell-crash-full-2026-10-16.md |
| src/ipc/shell_client.h | 2026-10-16 全文，见 code-audit-shell-crash-full-2026-10-16.md |
| src/ipc/shell_interaction_client.cpp | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
| src/ipc/shell_interaction_client.h | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
| src/ipc/shell_launch_protocol.h | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
| src/ipc/shell_query_client.cpp | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
| src/ipc/shell_query_client.h | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
| src/ipc/shell_query_protocol.h | 2026-10-16 全文，见 code-audit-small-ipc-2026-10-16.md |
