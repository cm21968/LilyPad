find_package(unofficial-sodium CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)

add_executable(lilypad_server
    main.cpp
    auth_db.cpp
    tls_config.cpp
    lilypad_server.rc
)

target_link_libraries(lilypad_server PRIVATE
    lilypad_common
    unofficial-sodium::sodium
    unofficial::sqlite3::sqlite3
)
