set(yaml_cpp_cmake "${YAML_CPP_SOURCE_DIR}/CMakeLists.txt")
file(READ "${yaml_cpp_cmake}" yaml_cpp_contents)
string(REPLACE
    "cmake_minimum_required(VERSION 3.4)"
    "cmake_minimum_required(VERSION 3.10)"
    yaml_cpp_contents
    "${yaml_cpp_contents}"
)
file(WRITE "${yaml_cpp_cmake}" "${yaml_cpp_contents}")
