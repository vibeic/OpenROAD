or_integration_tests(
  "fin"
  TESTS
    gcd_fill
    density_check_fixture
    density_window
    density_window_uncapped
    coupling_fill
    coupling_fill_off
    density_failsafe
    density_geometry_mismatch
)

add_subdirectory(cpp)

