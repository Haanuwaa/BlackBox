#pragma once

namespace blackbox::storage {

// Complete pre-release schema baseline. Only this version is accepted.
inline constexpr const char* archive_schema_v1 = R"sql(
CREATE TABLE schema_metadata (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1),
 schema_version INTEGER NOT NULL,
 created_utc_ms INTEGER NOT NULL);
CREATE TABLE incidents (
 id INTEGER PRIMARY KEY AUTOINCREMENT, created_utc_ms INTEGER NOT NULL,
 capture_sequence BLOB NOT NULL CHECK(length(capture_sequence)=8),
 event_monotonic_ns INTEGER NOT NULL, requested_start_ns INTEGER NOT NULL,
 requested_end_ns INTEGER NOT NULL, actual_start_ns INTEGER NOT NULL,
 actual_end_ns INTEGER NOT NULL, trigger_count INTEGER NOT NULL,
 system_recorder_epoch BLOB NOT NULL CHECK(length(system_recorder_epoch)=8),
 process_recorder_epoch BLOB NOT NULL CHECK(length(process_recorder_epoch)=8),
 event_recorder_epoch BLOB NOT NULL CHECK(length(event_recorder_epoch)=8),
 system_sample_count INTEGER NOT NULL, process_metadata_count INTEGER NOT NULL,
 process_sample_count INTEGER NOT NULL, system_event_count INTEGER NOT NULL,
 label TEXT NOT NULL DEFAULT '' CHECK(length(label)<=128),
 note TEXT NOT NULL DEFAULT '' CHECK(length(note)<=4096),
 manual_trigger_count INTEGER NOT NULL, automatic_trigger_count INTEGER NOT NULL,
 automatic_resource INTEGER NOT NULL CHECK(automatic_resource BETWEEN 0 AND 4),
    automatic_signal INTEGER NOT NULL CHECK(automatic_signal BETWEEN 0 AND 11),
 automatic_observed_value REAL NOT NULL, automatic_baseline_value REAL NOT NULL,
 automatic_score REAL NOT NULL,
 user_feedback INTEGER NOT NULL DEFAULT 0 CHECK(user_feedback BETWEEN 0 AND 2),
 category INTEGER NOT NULL DEFAULT 0 CHECK(category BETWEEN 0 AND 5),
 export_key BLOB NOT NULL CHECK(length(export_key)=16),
 recurring_group_override TEXT NOT NULL DEFAULT '' CHECK(length(recurring_group_override)<=64));
CREATE TABLE system_samples (
 incident_id INTEGER NOT NULL, sample_index INTEGER NOT NULL, observed_ns INTEGER NOT NULL,
 cpu_status INTEGER NOT NULL CHECK(cpu_status BETWEEN 0 AND 3),
 cpu_fraction REAL CHECK(cpu_status<>0 OR cpu_fraction IS NOT NULL),
 memory_used_status INTEGER NOT NULL CHECK(memory_used_status BETWEEN 0 AND 3),
 memory_used_bytes BLOB CHECK(memory_used_status<>0 OR (memory_used_bytes IS NOT NULL AND length(memory_used_bytes)=8)),
 memory_total_status INTEGER NOT NULL CHECK(memory_total_status BETWEEN 0 AND 3),
 memory_total_bytes BLOB CHECK(memory_total_status<>0 OR (memory_total_bytes IS NOT NULL AND length(memory_total_bytes)=8)),
 memory_fraction_status INTEGER NOT NULL CHECK(memory_fraction_status BETWEEN 0 AND 3),
 memory_fraction REAL CHECK(memory_fraction_status<>0 OR memory_fraction IS NOT NULL),
 disk_read_status INTEGER NOT NULL CHECK(disk_read_status BETWEEN 0 AND 3),
 disk_read_bps REAL CHECK(disk_read_status<>0 OR disk_read_bps IS NOT NULL),
 disk_write_status INTEGER NOT NULL CHECK(disk_write_status BETWEEN 0 AND 3),
 disk_write_bps REAL CHECK(disk_write_status<>0 OR disk_write_bps IS NOT NULL),
 network_receive_status INTEGER NOT NULL CHECK(network_receive_status BETWEEN 0 AND 3),
 network_receive_bps REAL CHECK(network_receive_status<>0 OR network_receive_bps IS NOT NULL),
 network_transmit_status INTEGER NOT NULL CHECK(network_transmit_status BETWEEN 0 AND 3),
 network_transmit_bps REAL CHECK(network_transmit_status<>0 OR network_transmit_bps IS NOT NULL),
 PRIMARY KEY(incident_id,sample_index),
 FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE TABLE system_quality_samples (
 incident_id INTEGER NOT NULL, sample_index INTEGER NOT NULL,
 disk_read_latency_status INTEGER NOT NULL CHECK(disk_read_latency_status BETWEEN 0 AND 3),
 disk_read_latency_seconds REAL CHECK(disk_read_latency_status<>0 OR disk_read_latency_seconds IS NOT NULL),
 disk_write_latency_status INTEGER NOT NULL CHECK(disk_write_latency_status BETWEEN 0 AND 3),
 disk_write_latency_seconds REAL CHECK(disk_write_latency_status<>0 OR disk_write_latency_seconds IS NOT NULL),
 disk_service_time_status INTEGER NOT NULL CHECK(disk_service_time_status BETWEEN 0 AND 3),
 disk_service_time_seconds REAL CHECK(disk_service_time_status<>0 OR disk_service_time_seconds IS NOT NULL),
 disk_queue_depth_status INTEGER NOT NULL CHECK(disk_queue_depth_status BETWEEN 0 AND 3),
 disk_queue_depth REAL CHECK(disk_queue_depth_status<>0 OR disk_queue_depth IS NOT NULL),
 disk_device_status INTEGER NOT NULL CHECK(disk_device_status BETWEEN 0 AND 3),
 disk_device_id BLOB CHECK(disk_device_status<>0 OR (disk_device_id IS NOT NULL AND length(disk_device_id)=8)),
 network_connectivity_status INTEGER NOT NULL CHECK(network_connectivity_status BETWEEN 0 AND 3),
 network_connectivity_level INTEGER CHECK(network_connectivity_status<>0 OR network_connectivity_level BETWEEN 0 AND 4),
 network_interfaces_status INTEGER NOT NULL CHECK(network_interfaces_status BETWEEN 0 AND 3),
 network_active_interfaces BLOB CHECK(network_interfaces_status<>0 OR (network_active_interfaces IS NOT NULL AND length(network_active_interfaces)=8)),
 network_changes_status INTEGER NOT NULL CHECK(network_changes_status BETWEEN 0 AND 3),
 network_interface_changes BLOB CHECK(network_changes_status<>0 OR (network_interface_changes IS NOT NULL AND length(network_interface_changes)=8)),
 tcp_retransmit_status INTEGER NOT NULL CHECK(tcp_retransmit_status BETWEEN 0 AND 3),
 tcp_retransmit_fraction REAL CHECK(tcp_retransmit_status<>0 OR tcp_retransmit_fraction IS NOT NULL),
 tcp_failures_status INTEGER NOT NULL CHECK(tcp_failures_status BETWEEN 0 AND 3),
 tcp_failed_connections BLOB CHECK(tcp_failures_status<>0 OR (tcp_failed_connections IS NOT NULL AND length(tcp_failed_connections)=8)),
 tcp_resets_status INTEGER NOT NULL CHECK(tcp_resets_status BETWEEN 0 AND 3),
 tcp_resets BLOB CHECK(tcp_resets_status<>0 OR (tcp_resets IS NOT NULL AND length(tcp_resets)=8)),
 PRIMARY KEY(incident_id,sample_index),
 FOREIGN KEY(incident_id,sample_index) REFERENCES system_samples(incident_id,sample_index) ON DELETE CASCADE);
CREATE TABLE system_extended_samples (
 incident_id INTEGER NOT NULL, sample_index INTEGER NOT NULL,
 gpu_status INTEGER NOT NULL CHECK(gpu_status BETWEEN 0 AND 3), gpu_fraction REAL CHECK(gpu_status<>0 OR gpu_fraction IS NOT NULL),
 gpu_dedicated_status INTEGER NOT NULL CHECK(gpu_dedicated_status BETWEEN 0 AND 3), gpu_dedicated_bytes BLOB CHECK(gpu_dedicated_status<>0 OR (gpu_dedicated_bytes IS NOT NULL AND length(gpu_dedicated_bytes)=8)),
 gpu_shared_status INTEGER NOT NULL CHECK(gpu_shared_status BETWEEN 0 AND 3), gpu_shared_bytes BLOB CHECK(gpu_shared_status<>0 OR (gpu_shared_bytes IS NOT NULL AND length(gpu_shared_bytes)=8)),
 foreground_status INTEGER NOT NULL CHECK(foreground_status BETWEEN 0 AND 3), foreground_pid INTEGER CHECK(foreground_status<>0 OR foreground_pid IS NOT NULL),
 foreground_creation_token BLOB CHECK(foreground_status<>0 OR (foreground_creation_token IS NOT NULL AND length(foreground_creation_token)=8)),
 foreground_gpu_status INTEGER NOT NULL CHECK(foreground_gpu_status BETWEEN 0 AND 3), foreground_gpu_fraction REAL CHECK(foreground_gpu_status<>0 OR foreground_gpu_fraction IS NOT NULL),
 dpc_status INTEGER NOT NULL CHECK(dpc_status BETWEEN 0 AND 3), dpc_fraction REAL CHECK(dpc_status<>0 OR dpc_fraction IS NOT NULL),
 interrupt_status INTEGER NOT NULL CHECK(interrupt_status BETWEEN 0 AND 3), interrupt_fraction REAL CHECK(interrupt_status<>0 OR interrupt_fraction IS NOT NULL),
 dpc_rate_status INTEGER NOT NULL CHECK(dpc_rate_status BETWEEN 0 AND 3), dpc_rate REAL CHECK(dpc_rate_status<>0 OR dpc_rate IS NOT NULL),
 cpu_current_status INTEGER NOT NULL CHECK(cpu_current_status BETWEEN 0 AND 3), cpu_current_mhz REAL CHECK(cpu_current_status<>0 OR cpu_current_mhz IS NOT NULL),
 cpu_max_status INTEGER NOT NULL CHECK(cpu_max_status BETWEEN 0 AND 3), cpu_max_mhz REAL CHECK(cpu_max_status<>0 OR cpu_max_mhz IS NOT NULL),
 cpu_limit_status INTEGER NOT NULL CHECK(cpu_limit_status BETWEEN 0 AND 3), cpu_limit_mhz REAL CHECK(cpu_limit_status<>0 OR cpu_limit_mhz IS NOT NULL),
 cpu_limit_fraction_status INTEGER NOT NULL CHECK(cpu_limit_fraction_status BETWEEN 0 AND 3), cpu_limit_fraction REAL CHECK(cpu_limit_fraction_status<>0 OR cpu_limit_fraction IS NOT NULL),
 power_source_status INTEGER NOT NULL CHECK(power_source_status BETWEEN 0 AND 3), power_source INTEGER CHECK(power_source_status<>0 OR power_source BETWEEN 0 AND 3),
 battery_status INTEGER NOT NULL CHECK(battery_status BETWEEN 0 AND 3), battery_fraction REAL CHECK(battery_status<>0 OR battery_fraction IS NOT NULL),
 battery_saver_status INTEGER NOT NULL CHECK(battery_saver_status BETWEEN 0 AND 3), battery_saver INTEGER CHECK(battery_saver_status<>0 OR battery_saver IN (0,1)),
 uptime_status INTEGER NOT NULL CHECK(uptime_status BETWEEN 0 AND 3), uptime_seconds REAL CHECK(uptime_status<>0 OR uptime_seconds IS NOT NULL),
 PRIMARY KEY(incident_id,sample_index),
 FOREIGN KEY(incident_id,sample_index) REFERENCES system_samples(incident_id,sample_index) ON DELETE CASCADE);
CREATE TABLE system_pressure_samples (
 incident_id INTEGER NOT NULL, sample_index INTEGER NOT NULL,
 cpu_some_status INTEGER NOT NULL CHECK(cpu_some_status BETWEEN 0 AND 3),
 cpu_some_fraction REAL CHECK(cpu_some_status<>0 OR cpu_some_fraction IS NOT NULL),
 memory_some_status INTEGER NOT NULL CHECK(memory_some_status BETWEEN 0 AND 3),
 memory_some_fraction REAL CHECK(memory_some_status<>0 OR memory_some_fraction IS NOT NULL),
 memory_full_status INTEGER NOT NULL CHECK(memory_full_status BETWEEN 0 AND 3),
 memory_full_fraction REAL CHECK(memory_full_status<>0 OR memory_full_fraction IS NOT NULL),
 io_some_status INTEGER NOT NULL CHECK(io_some_status BETWEEN 0 AND 3),
 io_some_fraction REAL CHECK(io_some_status<>0 OR io_some_fraction IS NOT NULL),
 io_full_status INTEGER NOT NULL CHECK(io_full_status BETWEEN 0 AND 3),
 io_full_fraction REAL CHECK(io_full_status<>0 OR io_full_fraction IS NOT NULL),
 thermal_pressure_status INTEGER NOT NULL CHECK(thermal_pressure_status BETWEEN 0 AND 3),
 thermal_pressure_state INTEGER CHECK(thermal_pressure_status<>0 OR thermal_pressure_state BETWEEN 0 AND 4),
 PRIMARY KEY(incident_id,sample_index),
 FOREIGN KEY(incident_id,sample_index) REFERENCES system_samples(incident_id,sample_index) ON DELETE CASCADE);
CREATE TABLE system_events (
 incident_id INTEGER NOT NULL, event_index INTEGER NOT NULL, observed_ns INTEGER NOT NULL,
    source_utc_ms INTEGER, source INTEGER NOT NULL CHECK(source BETWEEN 0 AND 10),
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 32), level INTEGER NOT NULL CHECK(level BETWEEN 0 AND 2),
 native_event_id INTEGER NOT NULL, detail INTEGER NOT NULL,
 has_process_identity INTEGER NOT NULL CHECK(has_process_identity IN (0,1)),
 process_pid INTEGER, process_creation_token BLOB,
  CHECK((source=10 AND kind IN (31,32) AND has_process_identity=1) OR
       (source<>10 AND kind NOT IN (31,32) AND has_process_identity=0)),
 CHECK((has_process_identity=0 AND process_pid IS NULL AND process_creation_token IS NULL) OR
       (has_process_identity=1 AND process_pid>0 AND process_creation_token IS NOT NULL AND length(process_creation_token)=8)),
 PRIMARY KEY(incident_id,event_index), FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE,
 FOREIGN KEY(incident_id,process_pid,process_creation_token)
  REFERENCES process_identities(incident_id,pid,creation_token) DEFERRABLE INITIALLY DEFERRED);
CREATE TABLE process_identities (
 incident_id INTEGER NOT NULL, pid INTEGER NOT NULL,
 creation_token BLOB NOT NULL CHECK(length(creation_token)=8),
 metadata_present INTEGER NOT NULL CHECK(metadata_present IN (0,1)),
 parent_pid_status INTEGER NOT NULL CHECK(parent_pid_status BETWEEN 0 AND 3),
 parent_pid INTEGER CHECK(parent_pid_status<>0 OR parent_pid IS NOT NULL),
 name_status INTEGER NOT NULL CHECK(name_status BETWEEN 0 AND 3), name TEXT CHECK(name_status<>0 OR name IS NOT NULL),
 path_status INTEGER NOT NULL CHECK(path_status BETWEEN 0 AND 3), executable_path TEXT CHECK(path_status<>0 OR executable_path IS NOT NULL),
 PRIMARY KEY(incident_id,pid,creation_token), FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE TABLE process_samples (
 incident_id INTEGER NOT NULL, sample_index INTEGER NOT NULL, observed_ns INTEGER NOT NULL,
 pid INTEGER NOT NULL, creation_token BLOB NOT NULL CHECK(length(creation_token)=8),
 cpu_status INTEGER NOT NULL CHECK(cpu_status BETWEEN 0 AND 3), cpu_fraction REAL CHECK(cpu_status<>0 OR cpu_fraction IS NOT NULL),
 working_set_status INTEGER NOT NULL CHECK(working_set_status BETWEEN 0 AND 3), working_set_bytes BLOB CHECK(working_set_status<>0 OR (working_set_bytes IS NOT NULL AND length(working_set_bytes)=8)),
 disk_read_status INTEGER NOT NULL CHECK(disk_read_status BETWEEN 0 AND 3), disk_read_bps REAL CHECK(disk_read_status<>0 OR disk_read_bps IS NOT NULL),
 disk_write_status INTEGER NOT NULL CHECK(disk_write_status BETWEEN 0 AND 3), disk_write_bps REAL CHECK(disk_write_status<>0 OR disk_write_bps IS NOT NULL),
 PRIMARY KEY(incident_id,sample_index), FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE,
 FOREIGN KEY(incident_id,pid,creation_token) REFERENCES process_identities(incident_id,pid,creation_token));
CREATE TABLE executable_profiles (
 executable_key TEXT PRIMARY KEY CHECK(length(executable_key) BETWEEN 1 AND 2048),
 display_name TEXT NOT NULL CHECK(length(display_name)<=512), last_seen_utc_ms INTEGER NOT NULL);
CREATE TABLE executable_profile_observations (
 executable_key TEXT NOT NULL, incident_id INTEGER NOT NULL, observed_utc_ms INTEGER NOT NULL,
 cpu_fraction REAL, working_set_bytes REAL, disk_read_bps REAL, disk_write_bps REAL,
 PRIMARY KEY(executable_key,incident_id), FOREIGN KEY(executable_key) REFERENCES executable_profiles(executable_key) ON DELETE CASCADE,
 FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE TABLE incident_classification_history (
 event_id INTEGER PRIMARY KEY AUTOINCREMENT, incident_id INTEGER NOT NULL, changed_utc_ms INTEGER NOT NULL,
 category INTEGER NOT NULL CHECK(category BETWEEN 0 AND 5), user_feedback INTEGER NOT NULL CHECK(user_feedback BETWEEN 0 AND 2),
 origin INTEGER NOT NULL CHECK(origin BETWEEN 0 AND 2), FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE TABLE incident_feature_cache (
 incident_id INTEGER NOT NULL, feature_version INTEGER NOT NULL CHECK(feature_version>0),
 feature_index INTEGER NOT NULL CHECK(feature_index BETWEEN 0 AND 31), value REAL NOT NULL,
 available INTEGER NOT NULL CHECK(available IN (0,1)), PRIMARY KEY(incident_id,feature_index),
 FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE TABLE feedback_profile_state (
 singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0),
 reset_after_utc_ms INTEGER NOT NULL CHECK(reset_after_utc_ms>=0),
 previous_reset_after_utc_ms INTEGER NOT NULL CHECK(previous_reset_after_utc_ms>=0),
 rollback_available INTEGER NOT NULL CHECK(rollback_available IN (0,1)));
CREATE TABLE incident_contributor_feedback (
 incident_id INTEGER NOT NULL, executable_key TEXT NOT NULL CHECK(length(executable_key) BETWEEN 1 AND 2048),
 resource INTEGER NOT NULL CHECK(resource BETWEEN 0 AND 3),
 disposition INTEGER NOT NULL CHECK(disposition BETWEEN 1 AND 2),
 temporal_relationship INTEGER NOT NULL CHECK(temporal_relationship BETWEEN 0 AND 2),
 updated_utc_ms INTEGER NOT NULL CHECK(updated_utc_ms>=0),
 PRIMARY KEY(incident_id,executable_key,resource),
 FOREIGN KEY(incident_id) REFERENCES incidents(id) ON DELETE CASCADE);
CREATE INDEX idx_incidents_created ON incidents(created_utc_ms DESC,id DESC);
CREATE UNIQUE INDEX idx_incidents_export_key ON incidents(export_key);
CREATE INDEX idx_incidents_label_created ON incidents(label COLLATE NOCASE,created_utc_ms DESC,id DESC);
CREATE INDEX idx_system_samples_observed ON system_samples(incident_id,observed_ns);
CREATE INDEX idx_system_events_observed ON system_events(incident_id,observed_ns);
CREATE INDEX idx_process_samples_observed ON process_samples(incident_id,observed_ns);
CREATE INDEX idx_process_samples_identity ON process_samples(incident_id,pid,creation_token);
CREATE INDEX idx_executable_profiles_last_seen ON executable_profiles(last_seen_utc_ms,executable_key);
CREATE INDEX idx_executable_observations_time ON executable_profile_observations(executable_key,observed_utc_ms DESC,incident_id DESC);
CREATE INDEX idx_incident_classification_history_time ON incident_classification_history(incident_id,changed_utc_ms,event_id);
CREATE INDEX idx_incident_feature_cache_version ON incident_feature_cache(feature_version,incident_id);
CREATE INDEX idx_contributor_feedback_history ON incident_contributor_feedback(executable_key,resource,incident_id);
INSERT INTO feedback_profile_state VALUES(1,0,0,0,0);
INSERT INTO schema_metadata VALUES(1,1,CAST(strftime('%s','now') AS INTEGER)*1000);
PRAGMA user_version=1;
PRAGMA application_id=1111644209;
)sql";

} // namespace blackbox::storage
