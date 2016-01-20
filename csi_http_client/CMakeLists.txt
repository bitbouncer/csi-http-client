SET(LIB_SRCS
	http_client.h
	http_client.cpp
	http_rest_avro_binary_encoding.h
	http_rest_avro_json_encoding.h
    )
	
add_library (csi-http-client STATIC ${LIB_SRCS})

