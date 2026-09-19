// WL-SVM / PE-P1-svm-specA
// Soft-margin SVM trained by projected dual gradient ascent (Gauss-Seidel),
// implemented from the frozen specification using only general-purpose
// language facilities (no linear-algebra / tensor / statistics / BLAS facility).
//
// Logical modules: data, svm, app.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// frozen constants
// ---------------------------------------------------------------------------
namespace cfg {

constexpr int    D          = 4;
constexpr int    N_TRAIN_C1 = 100;
constexpr int    N_TRAIN_C2 = 100;
constexpr int    N_TEST_C1  = 50;
constexpr int    N_TEST_C2  = 50;
constexpr int    N          = N_TRAIN_C1 + N_TRAIN_C2;   // 200
constexpr int    N_TEST     = N_TEST_C1 + N_TEST_C2;     // 100

constexpr double MU1[D] = { 1.0, 1.0, 0.5, -0.5 };
constexpr double MU2[D] = { -1.0, -1.0, -0.5, 0.5 };

constexpr double SIGMA   = 0.8;
constexpr long long SEED = 1234567;

constexpr double C      = 10.0;
constexpr double LR     = 0.0001;
constexpr double LIMIT  = 0.0001;
constexpr int    SWEEPS = 1000;
constexpr double EPS_SV = 0.0000001;

}  // namespace cfg

// ---------------------------------------------------------------------------
// module: data  -- LCG-PM generator and dataset builder
// ---------------------------------------------------------------------------
namespace data {

// Lehmer / Park-Miller "minimal standard" multiplicative generator.
//   state = (48271 * state) mod 2147483647
class LcgPm {
 public:
  explicit LcgPm(long long seed) : state_(seed) {}

  long long next_state() {
    state_ = (MULT * state_) % MOD;   // exact in int64 (max ~1.037e14)
    return state_;
  }

  double next_uniform() {
    return static_cast<double>(next_state()) / 2147483647.0;
  }

  // Irwin-Hall(12) - 6, accumulated in ascending order.
  double next_normal() {
    double t = 0.0;
    for (int k = 0; k < 12; ++k) {
      t = t + next_uniform();
    }
    return t - 6.0;
  }

 private:
  static constexpr long long MULT = 48271;
  static constexpr long long MOD  = 2147483647;
  long long state_;
};

struct Dataset {
  // training set: class +1 block first, class -1 block second
  std::vector<std::vector<double>> x;   // N x D
  std::vector<double>              y;   // N
  // test set: class +1 block first, class -1 block second
  std::vector<std::vector<double>> xt;  // N_TEST x D
  std::vector<double>              yt;  // N_TEST
};

// Draw one block of `count` points with mean `mu`, points ascending,
// dimensions ascending.
static void draw_block(LcgPm& rng, int count, const double mu[cfg::D],
                       double label, std::vector<std::vector<double>>& xs,
                       std::vector<double>& ys) {
  for (int n = 0; n < count; ++n) {
    std::vector<double> p(cfg::D, 0.0);
    for (int d = 0; d < cfg::D; ++d) {
      p[d] = mu[d] + cfg::SIGMA * rng.next_normal();
    }
    xs.push_back(p);
    ys.push_back(label);
  }
}

Dataset build() {
  Dataset ds;
  LcgPm rng(cfg::SEED);

  // exact draw order, one stream, never reset
  draw_block(rng, cfg::N_TRAIN_C1, cfg::MU1, 1.0, ds.x, ds.y);    // 1
  draw_block(rng, cfg::N_TRAIN_C2, cfg::MU2, -1.0, ds.x, ds.y);   // 2
  draw_block(rng, cfg::N_TEST_C1, cfg::MU1, 1.0, ds.xt, ds.yt);   // 3
  draw_block(rng, cfg::N_TEST_C2, cfg::MU2, -1.0, ds.xt, ds.yt);  // 4

  return ds;
}

}  // namespace data

// ---------------------------------------------------------------------------
// module: svm  -- training, SV extraction, w/b recovery, f, g, evaluation
// ---------------------------------------------------------------------------
namespace svm {

struct Model {
  std::vector<double> alpha;
  double beta          = 1.0;
  bool   judge_last    = false;
  double error_last    = 0.0;
  double max_abs_delta = 0.0;

  std::vector<int> s_margin;
  std::vector<int> s_inside;
  std::vector<double> w;
  double b = 0.0;
};

// Mandatory precomputation of the Gram matrix.
std::vector<std::vector<double>> gram(const std::vector<std::vector<double>>& x) {
  const int n = static_cast<int>(x.size());
  std::vector<std::vector<double>> g(n, std::vector<double>(n, 0.0));
  for (int i = 0; i < n; ++i) {
    for (int j = 0; j < n; ++j) {
      double s = 0.0;
      for (int d = 0; d < cfg::D; ++d) {
        s = s + x[i][d] * x[j][d];
      }
      g[i][j] = s;
    }
  }
  return g;
}

Model train(const std::vector<std::vector<double>>& x,
            const std::vector<double>& y) {
  const int n = static_cast<int>(x.size());
  const std::vector<std::vector<double>> g = gram(x);

  Model m;
  m.alpha.assign(n, 0.0);
  m.beta = 1.0;

  bool   judge         = false;
  double error         = 0.0;
  double max_abs_delta = 0.0;

  for (int sweep = 0; sweep < cfg::SWEEPS; ++sweep) {
    judge         = false;
    error         = 0.0;
    max_abs_delta = 0.0;

    // (3.1) update alpha in place, ascending i (Gauss-Seidel)
    for (int i = 0; i < n; ++i) {
      const double yi = y[i];
      const std::vector<double>& gi = g[i];

      double item1 = 0.0;
      for (int j = 0; j < n; ++j) {
        item1 = item1 + m.alpha[j] * yi * y[j] * gi[j];
      }

      double item2 = 0.0;
      for (int j = 0; j < n; ++j) {
        item2 = item2 + m.alpha[j] * yi * y[j];
      }

      const double delta = 1.0 - item1 - m.beta * item2;
      const double ad    = delta < 0.0 ? -delta : delta;

      if (ad > max_abs_delta) max_abs_delta = ad;

      m.alpha[i] = m.alpha[i] + cfg::LR * delta;
      if (m.alpha[i] < 0.0) {
        m.alpha[i] = 0.0;
      } else if (m.alpha[i] > cfg::C) {
        m.alpha[i] = cfg::C;
      } else if (ad > cfg::LIMIT) {
        judge = true;
        error = error + (ad - cfg::LIMIT);
      }
    }

    // (3.2) update beta once per sweep
    double s = 0.0;
    for (int i = 0; i < n; ++i) {
      s = s + m.alpha[i] * y[i];
    }
    m.beta = m.beta + s * s / 2.0;
  }

  m.judge_last    = judge;
  m.error_last    = error;
  m.max_abs_delta = max_abs_delta;
  return m;
}

void recover(Model& m, const std::vector<std::vector<double>>& x,
             const std::vector<double>& y) {
  const int n = static_cast<int>(x.size());

  m.s_margin.clear();
  m.s_inside.clear();
  for (int i = 0; i < n; ++i) {
    if (cfg::EPS_SV < m.alpha[i] && m.alpha[i] < cfg::C - cfg::EPS_SV) {
      m.s_margin.push_back(i);
    }
  }
  for (int i = 0; i < n; ++i) {
    if (m.alpha[i] >= cfg::C - cfg::EPS_SV) {
      m.s_inside.push_back(i);
    }
  }

  m.w.assign(cfg::D, 0.0);
  for (int d = 0; d < cfg::D; ++d) {
    for (size_t k = 0; k < m.s_margin.size(); ++k) {
      const int i = m.s_margin[k];
      m.w[d] = m.w[d] + m.alpha[i] * y[i] * x[i][d];
    }
    for (size_t k = 0; k < m.s_inside.size(); ++k) {
      const int i = m.s_inside[k];
      m.w[d] = m.w[d] + m.alpha[i] * y[i] * x[i][d];
    }
  }

  double b = 0.0;
  for (size_t k = 0; k < m.s_margin.size(); ++k) {
    const int i = m.s_margin[k];
    double dp = 0.0;
    for (int d = 0; d < cfg::D; ++d) {
      dp = dp + m.w[d] * x[i][d];
    }
    b = b + (y[i] - dp);
  }
  if (m.s_margin.empty()) {
    std::fprintf(stderr, "FAIL: |S_margin| == 0\n");
    std::exit(1);
  }
  m.b = b / static_cast<double>(m.s_margin.size());
}

double f(const Model& m, const std::vector<double>& p) {
  double s = 0.0;
  for (int d = 0; d < cfg::D; ++d) {
    s = s + m.w[d] * p[d];
  }
  return s + m.b;
}

int g_sign(const Model& m, const std::vector<double>& p) {
  return f(m, p) >= 0.0 ? 1 : -1;
}

double objective(const std::vector<double>& alpha, const std::vector<double>& y,
                 const std::vector<std::vector<double>>& gm) {
  const int n = static_cast<int>(alpha.size());
  double sum_a = 0.0;
  for (int i = 0; i < n; ++i) {
    sum_a = sum_a + alpha[i];
  }
  double quad = 0.0;
  for (int i = 0; i < n; ++i) {
    double inner = 0.0;
    for (int j = 0; j < n; ++j) {
      inner = inner + alpha[i] * alpha[j] * y[i] * y[j] * gm[i][j];
    }
    quad = quad + inner;
  }
  return sum_a - 0.5 * quad;
}

double alpha_checksum(const std::vector<double>& alpha) {
  const int n = static_cast<int>(alpha.size());
  double s = 0.0;
  for (int i = 0; i < n; ++i) {
    s = s + alpha[i] * static_cast<double>((i % 97) + 1);
  }
  return s;
}

}  // namespace svm

// ---------------------------------------------------------------------------
// module: app  -- entry point and output formatting
// ---------------------------------------------------------------------------
namespace app {

// Fixed-point formatting with the negative-zero normalization required by 1.4.
std::string fixed(double v, int digits) {
  if (v == 0.0) v = 0.0;  // maps -0.0 to +0.0
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.*f", digits, v);
  std::string s(buf);
  // guard against "-0.000000" produced by rounding a tiny negative value
  bool all_zero = true;
  for (size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '1' && c <= '9') { all_zero = false; break; }
  }
  if (all_zero && !s.empty() && s[0] == '-') s.erase(s.begin());
  return s;
}

std::string f6(double v)  { return fixed(v, 6); }
std::string f12(double v) { return fixed(v, 12); }

int run() {
  const data::Dataset ds = data::build();

  svm::Model m = svm::train(ds.x, ds.y);
  svm::recover(m, ds.x, ds.y);

  const std::vector<std::vector<double>> gm = svm::gram(ds.x);

  // evaluation
  int train_correct = 0;
  for (int i = 0; i < cfg::N; ++i) {
    const int pred = svm::g_sign(m, ds.x[i]);
    if (static_cast<double>(pred) == ds.y[i]) ++train_correct;
  }

  std::vector<int> pred(cfg::N_TEST, 0);
  int correct_c1 = 0, correct_c2 = 0;
  for (int i = 0; i < cfg::N_TEST; ++i) {
    pred[i] = svm::g_sign(m, ds.xt[i]);
    if (static_cast<double>(pred[i]) == ds.yt[i]) {
      if (i < cfg::N_TEST_C1) ++correct_c1; else ++correct_c2;
    }
  }
  const int test_correct = correct_c1 + correct_c2;

  double alpha_sum = 0.0;
  for (int i = 0; i < cfg::N; ++i) alpha_sum = alpha_sum + m.alpha[i];
  double alpha_y_sum = 0.0;
  for (int i = 0; i < cfg::N; ++i) alpha_y_sum = alpha_y_sum + m.alpha[i] * ds.y[i];

  const double obj  = svm::objective(m.alpha, ds.y, gm);
  const double chks = svm::alpha_checksum(m.alpha);

  const double train_acc = static_cast<double>(train_correct) / static_cast<double>(cfg::N);
  const double test_acc  = static_cast<double>(test_correct) / static_cast<double>(cfg::N_TEST);
  const double test_acc_c1 = static_cast<double>(correct_c1) / static_cast<double>(cfg::N_TEST_C1);
  const double test_acc_c2 = static_cast<double>(correct_c2) / static_cast<double>(cfg::N_TEST_C2);

  std::string out;
  out += "SVM_VERSION 1\n";
  out += "SWEEPS " + std::to_string(cfg::SWEEPS) + "\n";
  out += "CONVERGED " + std::string(m.judge_last ? "0" : "1") + "\n";
  out += "ERROR_LAST " + f6(m.error_last) + "\n";
  out += "MAX_ABS_DELTA " + f6(m.max_abs_delta) + "\n";
  out += "BETA " + f6(m.beta) + "\n";
  out += "NS_MARGIN " + std::to_string(m.s_margin.size()) + "\n";
  out += "NS_INSIDE " + std::to_string(m.s_inside.size()) + "\n";
  out += "W";
  for (int d = 0; d < cfg::D; ++d) out += " " + f6(m.w[d]);
  out += "\n";
  out += "B " + f6(m.b) + "\n";
  out += "OBJECTIVE " + f6(obj) + "\n";
  out += "ALPHA_SUM " + f6(alpha_sum) + "\n";
  out += "ALPHA_Y_SUM " + f12(alpha_y_sum) + "\n";
  out += "ALPHA_CHECKSUM " + f6(chks) + "\n";
  out += "TRAIN_ACC " + f6(train_acc) + "\n";
  out += "TEST_ACC " + f6(test_acc) + "\n";
  out += "TEST_ACC_C1 " + f6(test_acc_c1) + "\n";
  out += "TEST_ACC_C2 " + f6(test_acc_c2) + "\n";
  out += "TEST_CORRECT " + std::to_string(correct_c1) + " " +
         std::to_string(correct_c2) + " " + std::to_string(test_correct) + "\n";
  out += "PRED";
  for (int i = 0; i < cfg::N_TEST; ++i) out += " " + std::to_string(pred[i]);
  out += "\n";
  out += "ALPHA";
  for (int i = 0; i < cfg::N; ++i) out += " " + f6(m.alpha[i]);
  out += "\n";

  std::fwrite(out.data(), 1, out.size(), stdout);
  return 0;
}

}  // namespace app

int main() {
  return app::run();
}
