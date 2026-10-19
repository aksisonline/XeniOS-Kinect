add_library(xenia-hid-kinect STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera.h
)

if(WIN32)
  target_sources(xenia-hid-kinect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera_libusb.cc
  )
  target_link_libraries(xenia-hid-kinect PUBLIC libusb)
elseif(APPLE AND NOT XE_PLATFORM_IOS)
  # ponytail: system libusb (brew install libusb); vendor the darwin backend
  # in third_party before shipping a self-contained .app.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
  target_sources(xenia-hid-kinect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera_libusb.cc
  )
  target_link_libraries(xenia-hid-kinect PUBLIC PkgConfig::LIBUSB)
else()
  target_sources(xenia-hid-kinect PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/usb_camera_posix.cc
  )
endif()
target_link_libraries(xenia-hid-kinect PUBLIC xenia-base)
xe_target_defaults(xenia-hid-kinect)
