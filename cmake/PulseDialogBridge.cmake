# SPDX-License-Identifier: Apache-2.0

add_library(pulse_picker_wire STATIC
    src/common/picker_owner_guard.cpp
    src/ipc/picker_client.cpp
    src/ipc/picker_protocol.cpp
    src/ipc/picker_transport.cpp)
target_link_libraries(pulse_picker_wire PUBLIC advapi32 bcrypt user32)

add_library(pulse_picker_save STATIC
    src/ui/file_save_model.cpp
    src/ui/file_save_revalidation.cpp
    src/ui/file_save_access_policy.cpp)
target_link_libraries(pulse_picker_save PUBLIC advapi32)

add_library(pulse_file_dialog_registration STATIC src/common/file_dialog_registration.cpp)
target_link_libraries(pulse_file_dialog_registration PUBLIC advapi32 ole32 uuid)

add_library(pulse_dialog_bridge SHARED
    src/shell_bridge/file_dialog_auto_server.cpp
    src/shell_bridge/file_dialog_auto_server.def
    src/shell_bridge/file_dialog_class_factory.cpp
    src/shell_bridge/file_dialog_client_state.cpp
    src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/file_dialog_events.cpp
    src/shell_bridge/file_dialog_wrapper.cpp
    src/shell_bridge/picker_shell_results.cpp)
set_target_properties(pulse_dialog_bridge PROPERTIES OUTPUT_NAME Pulse.DialogBridge)
target_link_libraries(pulse_dialog_bridge PRIVATE
    pulse_picker_wire pulse_picker_save pulse_file_dialog_registration
    shell32 ole32 uuid user32 propsys)
