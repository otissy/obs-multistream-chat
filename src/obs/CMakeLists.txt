# OBS plugin module. Headers and import libs come from the vendored SDKs (see PROJECT.md):
#   .obs-sdk/{libobs,frontend-api};  ${OMS_IMPORT_LIB_DIR}/{obs.lib,obs-frontend-api.lib} (generated, central build folder)
#   .qt-sdk/{include/Qt*,lib/Qt6{Core,Gui,Widgets}.lib}
foreach(req "${OBS_SDK_DIR}/libobs/obs-module.h" "${QT_SDK_DIR}/include/QtWidgets/QWidget" "${QT_SDK_DIR}/lib/Qt6Widgets.lib")
  if(NOT EXISTS "${req}")
    message(FATAL_ERROR "Missing ${req}. Run scripts/fetch-sdks.ps1 (see README).")
  endif()
endforeach()
set(OMS_HAVE_IMPORT_LIBS ON)
foreach(req "${OMS_IMPORT_LIB_DIR}/obs.lib" "${OMS_IMPORT_LIB_DIR}/obs-frontend-api.lib")
  if(NOT EXISTS "${req}")
    set(OMS_HAVE_IMPORT_LIBS OFF)
  endif()
endforeach()

if(OMS_HAVE_IMPORT_LIBS)
  add_library(obs-multistream-chat MODULE plugin-main.cpp output_manager.cpp dock.cpp chat_panel.cpp reader_process.cpp)
else()
  message(WARNING "obs.lib / obs-frontend-api.lib not found in ${OMS_IMPORT_LIB_DIR}: compiling the plugin sources only "
                  "(no DLL). Run scripts/make-import-lib.ps1 to produce the DLL.")
  add_library(obs-multistream-chat OBJECT plugin-main.cpp output_manager.cpp dock.cpp chat_panel.cpp reader_process.cpp)
endif()
target_include_directories(obs-multistream-chat PRIVATE
  "${OBS_SDK_DIR}/libobs" "${OBS_SDK_DIR}/frontend-api"
  "${QT_SDK_DIR}/include" "${QT_SDK_DIR}/include/QtCore" "${QT_SDK_DIR}/include/QtGui" "${QT_SDK_DIR}/include/QtWidgets")
target_link_libraries(obs-multistream-chat PRIVATE oms_core)
if(OMS_HAVE_IMPORT_LIBS)
  target_link_libraries(obs-multistream-chat PRIVATE
    "${OMS_IMPORT_LIB_DIR}/obs.lib" "${OMS_IMPORT_LIB_DIR}/obs-frontend-api.lib"
    version.lib winmm.lib ole32.lib wininet.lib "${QT_SDK_DIR}/lib/Qt6Core.lib" "${QT_SDK_DIR}/lib/Qt6Gui.lib" "${QT_SDK_DIR}/lib/Qt6Widgets.lib")
  endif()
target_compile_definitions(obs-multistream-chat PRIVATE PLUGIN_VERSION="${PROJECT_VERSION}" QT_NO_DEBUG QT_NO_KEYWORDS)
set_target_properties(obs-multistream-chat PROPERTIES PREFIX "" AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)
oms_add_version_info(obs-multistream-chat "Multistream to several RTMP(S) destinations for OBS Studio (no platform APIs)" VFT_DLL .dll)
