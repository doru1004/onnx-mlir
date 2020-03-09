configure_file(driver.py driver.py COPYONLY)
configure_file(driver_config.py.in driver_config.py)

find_package(PythonInterp 3 REQUIRED)

