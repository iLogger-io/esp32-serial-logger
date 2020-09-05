idf_component_register(SRCS "src/main.c"
    "src/local_webserver_apsta.c"
    "src/captdns.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "public/index.html"
    EMBED_FILES "public/wifi1.svg"
    EMBED_FILES "public/wifi2.svg"
    EMBED_FILES "public/wifi3.svg"
    EMBED_FILES "public/wififull.svg")