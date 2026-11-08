# Boot and the home screen. Without REQUIRES, ESP-IDF makes main require every component in the build: the OS
# (components/os) and every app (apps/<name>, listed in apps.c).
idf_component_register(SRCS "main.c" "home.c" "apps.c")
