configure_file(onnf-run ${CMAKE_BINARY_DIR}/bin/onnf-run COPYONLY)

configure_file(__init__.py ${CMAKE_BINARY_DIR}/bin/python/__init__.py COPYONLY)
configure_file(driver.py ${CMAKE_BINARY_DIR}/bin/python/driver.py)
configure_file(driver_config.py.in ${CMAKE_BINARY_DIR}/bin/python/driver_config.py)

find_package(PythonInterp 3 REQUIRED)
