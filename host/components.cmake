# Pure (hardware-independent) components built on the host. Order does not matter.
# Keep in sync with docs/COMPONENTS.md (tools/check_deps.py verifies this).
set(QZ_HOST_COMPONENTS
  qz_core
  qz_hal
  qz_board
  qz_time
  qz_model
  qz_gfx
  qz_ssd1681
  qz_bma423
  qz_settings
  qz_steps
  qz_power
  qz_weather
  qz_conn
  qz_ui
  qz_faces
  qz_console
  qz_selftest
  qz_app
  qz_testkit)
