# The pipeline orchestration (C++ port of the native app's pipeline.py, detect.py,
# fec.py, interleave.py, correlate.py, io.py, demod.py), plus the SIH26147 extra
# features (raster.cpp is Feature 4, the frame raster view; rank_analysis.cpp is
# Feature 2, the rank-analysis view). Plain C++ on top of sigcore_core: no Qt, no
# Embind, so it builds and is tested natively and under WASM. The Qt GUI (../) and
# the Embind bridge (/bridge) both link it.

add_library(sigpipeline STATIC
  src/signal_io.cpp
  src/demod_dispatch.cpp
  src/interleave_dispatch.cpp
  src/fec_dispatch.cpp
  src/correlate.cpp
  src/detect.cpp
  src/raster.cpp
  src/rank_analysis.cpp
  src/pipeline.cpp
  src/result_text.cpp
)
target_include_directories(sigpipeline PUBLIC include)
target_link_libraries(sigpipeline PUBLIC sigcore_core)
set_target_properties(sigpipeline PROPERTIES POSITION_INDEPENDENT_CODE ON)
