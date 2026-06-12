build_lib_example(
  NAME diffserv-example-1
  SOURCE_FILES diffserv-example-1.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-example-2
  SOURCE_FILES diffserv-example-2.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-example-3
  SOURCE_FILES diffserv-example-3.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME gold-policer-replay
  SOURCE_FILES gold-policer-replay.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME chang-comparison
  SOURCE_FILES chang-comparison.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-example-le
  SOURCE_FILES diffserv-example-le.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-fqcodel-comparison
  SOURCE_FILES diffserv-l4s-fqcodel-comparison.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-s1-latency
  SOURCE_FILES diffserv-l4s-s1-latency.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-s2-equivalence
  SOURCE_FILES diffserv-l4s-s2-equivalence.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-s2-coexistence
  SOURCE_FILES diffserv-l4s-s2-coexistence.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-dualpi2-gprt-parity
  SOURCE_FILES diffserv-l4s-dualpi2-gprt-parity.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-l4s-s1-advantage
  SOURCE_FILES diffserv-l4s-s1-advantage.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-hierarchical-l4s
  SOURCE_FILES diffserv-hierarchical-l4s.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME retx-calibration
  SOURCE_FILES retx-calibration.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-cake
  SOURCE_FILES diffserv-cake.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libflow-monitor}
    ${libstratum}
)

build_lib_example(
  NAME cake-rrul
  SOURCE_FILES cake-rrul.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libinternet-apps}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME cake-tcp-4up-squarewave
  SOURCE_FILES cake-tcp-4up-squarewave.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libinternet-apps}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME cake-flent-host-attribution-smoke
  SOURCE_FILES cake-flent-host-attribution-smoke.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libinternet-apps}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME cake-rrul-diffserv
  SOURCE_FILES cake-rrul-diffserv.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libinternet-apps}
    ${libapplications}
    ${libtraffic-control}
    ${libstratum}
)

build_lib_example(
  NAME aqm-eval-runner
  SOURCE_FILES
    aqm-eval-runner.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libflow-monitor}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-wifi-demo
  SOURCE_FILES diffserv-wifi-demo.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libwifi}
    ${libmobility}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-wifi-scheduler-comparison
  SOURCE_FILES diffserv-wifi-scheduler-comparison.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libflow-monitor}
    ${libwifi}
    ${libmobility}
    ${libstratum}
)

build_lib_example(
  NAME diffserv-hybrid-wired-wireless
  SOURCE_FILES diffserv-hybrid-wired-wireless.cc
  LIBRARIES_TO_LINK
    ${libpoint-to-point}
    ${libinternet}
    ${libapplications}
    ${libtraffic-control}
    ${libflow-monitor}
    ${libwifi}
    ${libmobility}
    ${libstratum}
)

find_file(
  FD_NET_DEVICE_MODULE_H
  NAMES fd-net-device-module.h
  PATHS "${CMAKE_OUTPUT_DIRECTORY}/include/ns3"
        "${PROJECT_BINARY_DIR}/include/ns3"
  NO_DEFAULT_PATH
)
if(FD_NET_DEVICE_MODULE_H)
  build_lib_example(
    NAME cake-stratum-bridge-router
    SOURCE_FILES cake-stratum-bridge-router.cc
    LIBRARIES_TO_LINK
      ${libcore}
      ${libnetwork}
      ${libinternet}
      ${libtraffic-control}
      ${libfd-net-device}
      ${libstratum}
  )
endif()

build_lib_example(
  NAME cake-l4s-composition
  SOURCE_FILES cake-l4s-composition.cc
  LIBRARIES_TO_LINK
    ${libcore}
    ${libnetwork}
    ${libinternet}
    ${libtraffic-control}
    ${libpoint-to-point}
    ${libapplications}
    ${libstratum}
)
