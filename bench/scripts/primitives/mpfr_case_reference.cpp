#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

#include <gmp.h>
#include <mpfr.h>

namespace {

mpfr_prec_t precision_for_kind(const std::string& kind) {
  if (kind == "f128") {
    return 113;
  }
  if (kind == "f256") {
    return 237;
  }
  if (kind == "f512") {
    return 493;
  }
  return 64;
}

int digits_for_precision(mpfr_prec_t precision) {
  const double bits = static_cast<double>(precision);
  const int digits = static_cast<int>(std::ceil(bits * 0.3010299956639812));
  return std::max(24, digits + 6);
}

void mpfr_set_from_decimal_or_zero(mpfr_t out, const std::string& text) {
  if (text.empty()) {
    mpfr_set_ui(out, 0U, MPFR_RNDN);
    return;
  }
  if (mpfr_set_str(out, text.c_str(), 10, MPFR_RNDN) == 0) {
    return;
  }
  mpfr_set_ui(out, 0U, MPFR_RNDN);
}

std::string mpfr_to_decimal_string(const mpfr_t value, int digits) {
  char* buffer = nullptr;
  mpfr_asprintf(&buffer, "%.*Rg", digits, value);
  std::string out = buffer ? std::string(buffer) : std::string("0");
  if (buffer) {
    mpfr_free_str(buffer);
  }
  return out;
}

bool apply_op(const std::string& op, const mpfr_t a, const mpfr_t b, mpfr_t out) {
  if (op == "+") {
    mpfr_add(out, a, b, MPFR_RNDN);
    return true;
  }
  if (op == "-") {
    mpfr_sub(out, a, b, MPFR_RNDN);
    return true;
  }
  if (op == "*") {
    mpfr_mul(out, a, b, MPFR_RNDN);
    return true;
  }
  if (op == "/") {
    if (mpfr_zero_p(b) != 0) {
      return false;
    }
    mpfr_div(out, a, b, MPFR_RNDN);
    return true;
  }
  if (op == "%") {
    if (mpfr_zero_p(b) != 0) {
      return false;
    }
    mpfr_fmod(out, a, b, MPFR_RNDN);
    return true;
  }
  if (op == "^") {
    mpfr_pow(out, a, b, MPFR_RNDN);
    return true;
  }
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: mpfr_case_reference <primitive> <operator>\n";
    return 1;
  }

  const std::string primitive = argv[1];
  const std::string op = argv[2];
  const auto precision = precision_for_kind(primitive);
  const int digits = digits_for_precision(precision);

  mpfr_t a;
  mpfr_t b;
  mpfr_t out;
  mpfr_init2(a, precision);
  mpfr_init2(b, precision);
  mpfr_init2(out, precision);

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) {
      continue;
    }
    const auto sep = line.find('|');
    if (sep == std::string::npos || sep == 0 || sep + 1 >= line.size()) {
      std::cerr << "invalid row: " << line << "\n";
      mpfr_clear(a);
      mpfr_clear(b);
      mpfr_clear(out);
      return 2;
    }
    const std::string a_text = line.substr(0, sep);
    const std::string b_text = line.substr(sep + 1);
    mpfr_set_from_decimal_or_zero(a, a_text);
    mpfr_set_from_decimal_or_zero(b, b_text);

    if (!apply_op(op, a, b, out)) {
      std::cout << "nan\n";
      continue;
    }
    std::cout << mpfr_to_decimal_string(out, digits) << "\n";
  }

  mpfr_clear(a);
  mpfr_clear(b);
  mpfr_clear(out);
  return 0;
}

