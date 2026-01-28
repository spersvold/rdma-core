
rdma_shared_provider(nerve libnerve.map
	1 1.4.${PACKAGE_VERSION}
	nerve.c
	verbs.c
)

publish_headers(infiniband
	nervedv.h
)

rdma_pkg_config("nerve" "libibverbs" "${CMAKE_THREAD_LIBS_INIT}")
