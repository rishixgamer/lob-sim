add_library(faircross_continuous STATIC event.cpp order_book.cpp matching_engine.cpp replay.cpp)
target_compile_features(faircross_continuous PUBLIC cxx_std_20)
target_include_directories(faircross_continuous PUBLIC ../../include)
set_target_properties(faircross_continuous PROPERTIES POSITION_INDEPENDENT_CODE ON)
