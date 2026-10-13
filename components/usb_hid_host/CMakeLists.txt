idf_component_register(SRCS "usb_hid_host.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES usb usb_host_hid)
