#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <map>
#include <optional>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "spark/ast.h"
#include "spark/codegen.h"
#include "spark/cpu_features.h"
#include "spark/evaluator.h"
#include "spark/parser.h"
#include "spark/semantic.h"

// Modüler CLI düzenlemesi: sparkc_main fonksiyonları fonksiyonel bloklara ayrıldı.
// Her parça tek sorumlulukta: ortak veri yapıları, pipeline, çalışma akışı, komut ayrıştırma.
namespace {

#include "sparkc_main_parts/01_common.cpp"
#include "sparkc_main_parts/02_pipeline.cpp"
#include "sparkc_main_parts/03_execute.cpp"

}  // namespace

#include "sparkc_main_parts/04_main.cpp"
