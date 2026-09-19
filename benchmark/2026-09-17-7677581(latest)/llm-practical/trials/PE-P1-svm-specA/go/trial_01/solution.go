// WL-SVM / PE-P1-svm-specA — soft-margin SVM trained by projected dual ascent.
//
// Single file, standard library only (fmt, os, strings).
// No linear-algebra / tensor / statistics / BLAS facility is used: every
// accumulation below is a hand-written scalar loop (frozen rule C-9).
//
// Three logical modules, as required by spec 2.1:
//   data — the LCG-PM generator and the dataset builder
//   svm  — training, support-vector extraction, w/b recovery, f, g, evaluation
//   app  — entry point and output formatting
package main

import (
	"bufio"
	"fmt"
	"os"
)

// ---------------------------------------------------------------------------
// Frozen constants (spec 2.2)
// ---------------------------------------------------------------------------

const (
	D          = 4
	NTrainC1   = 100
	NTrainC2   = 100
	NTestC1    = 50
	NTestC2    = 50
	Sigma      = 0.8
	Seed       = 1234567
	C          = 10.0
	LR         = 0.0001
	Limit      = 0.0001
	Sweeps     = 1000
	EpsSV      = 0.0000001
	NTrain     = NTrainC1 + NTrainC2 // 200
	NTest      = NTestC1 + NTestC2   // 100
	SVMVersion = 1
)

var (
	MU1 = [D]float64{1.0, 1.0, 0.5, -0.5}
	MU2 = [D]float64{-1.0, -1.0, -0.5, 0.5}
)

// ===========================================================================
// module: data
// ===========================================================================

// lcgPM is the Park–Miller minimal-standard LCG (cross-language constraint C-1):
//
//	state_{n+1} = (state_n * 48271) mod 2147483647
//	unit_float  = state_{n+1} / 2147483647.0
//
// The largest intermediate, 48271 * 2147483646 ~ 1.037e14, is exact in int64.
type lcgPM struct {
	state int64
}

func newLCGPM(seed int64) *lcgPM {
	return &lcgPM{state: seed}
}

// nextUnit advances the stream and returns the next unit float in (0,1).
func (r *lcgPM) nextUnit() float64 {
	r.state = (r.state * 48271) % 2147483647
	return float64(r.state) / 2147483647.0
}

// nextNormal returns an Irwin–Hall(12) standard normal deviate:
// the sum of twelve consecutive unit floats, minus six.
func (r *lcgPM) nextNormal() float64 {
	s := 0.0
	for k := 0; k < 12; k++ {
		s = s + r.nextUnit()
	}
	return s - 6.0
}

// fillBlock draws n points of D dimensions around mu, points ascending and,
// within a point, dimensions ascending.
func fillBlock(r *lcgPM, n int, mu [D]float64) [][D]float64 {
	block := make([][D]float64, n)
	for i := 0; i < n; i++ {
		for d := 0; d < D; d++ {
			block[i][d] = mu[d] + Sigma*r.nextNormal()
		}
	}
	return block
}

type dataset struct {
	xTrain []([D]float64) // 200 training points, class +1 block then class -1 block
	yTrain []float64      // +1 for i < 100, -1 for i >= 100
	xTest1 []([D]float64) // 50 test points of class +1
	xTest2 []([D]float64) // 50 test points of class -1
}

// buildDataset draws the four blocks from one stream, in the exact order of
// spec 2.3, and concatenates the training blocks class +1 first.
func buildDataset() *dataset {
	r := newLCGPM(Seed)

	train1 := fillBlock(r, NTrainC1, MU1)
	train2 := fillBlock(r, NTrainC2, MU2)
	test1 := fillBlock(r, NTestC1, MU1)
	test2 := fillBlock(r, NTestC2, MU2)

	xTrain := make([][D]float64, 0, NTrain)
	yTrain := make([]float64, 0, NTrain)
	for i := 0; i < NTrainC1; i++ {
		xTrain = append(xTrain, train1[i])
		yTrain = append(yTrain, 1.0)
	}
	for i := 0; i < NTrainC2; i++ {
		xTrain = append(xTrain, train2[i])
		yTrain = append(yTrain, -1.0)
	}

	return &dataset{xTrain: xTrain, yTrain: yTrain, xTest1: test1, xTest2: test2}
}

// ===========================================================================
// module: svm
// ===========================================================================

type model struct {
	alpha        []float64
	beta         float64
	judge        bool
	errorLast    float64
	maxAbsDelta  float64
	w            [D]float64
	b            float64
	nsMargin     int
	nsInside     int
	sMargin      []int
	sInside      []int
	objective    float64
	alphaSum     float64
	alphaYSum    float64
	alphaChecksm float64
}

// gram builds the Gram matrix once. Precomputing is mandatory (spec 2.4).
func gram(x [][D]float64) [][]float64 {
	n := len(x)
	g := make([][]float64, n)
	for i := 0; i < n; i++ {
		g[i] = make([]float64, n)
	}
	for i := 0; i < n; i++ {
		for j := 0; j < n; j++ {
			s := 0.0
			for d := 0; d < D; d++ {
				s = s + x[i][d]*x[j][d]
			}
			g[i][j] = s
		}
	}
	return g
}

// train runs exactly SWEEPS full sweeps of projected dual ascent, with no
// early exit. alpha is updated in place (Gauss–Seidel), beta once per sweep.
func train(g [][]float64, y []float64) *model {
	n := len(y)

	alpha := make([]float64, n)
	for i := 0; i < n; i++ {
		alpha[i] = 0.0
	}
	beta := 1.0

	judge := false
	errAcc := 0.0
	maxAbsDelta := 0.0

	for sweep := 0; sweep < Sweeps; sweep++ {

		judge = false
		errAcc = 0.0
		maxAbsDelta = 0.0

		// (3.1) update alpha, ascending i, in place
		for i := 0; i < n; i++ {

			item1 := 0.0
			for j := 0; j < n; j++ {
				item1 = item1 + alpha[j]*y[i]*y[j]*g[i][j]
			}

			item2 := 0.0
			for j := 0; j < n; j++ {
				item2 = item2 + alpha[j]*y[i]*y[j]
			}

			delta := 1.0 - item1 - beta*item2

			ad := delta
			if ad < 0.0 {
				ad = -ad
			}
			if ad > maxAbsDelta {
				maxAbsDelta = ad
			}

			alpha[i] = alpha[i] + LR*delta
			if alpha[i] < 0.0 {
				alpha[i] = 0.0
			} else if alpha[i] > C {
				alpha[i] = C
			} else if ad > Limit {
				judge = true
				errAcc = errAcc + (ad - Limit)
			}
		}

		// (3.2) update beta, once per sweep
		s := 0.0
		for i := 0; i < n; i++ {
			s = s + alpha[i]*y[i]
		}
		beta = beta + s*s/2.0
	}

	return &model{
		alpha:       alpha,
		beta:        beta,
		judge:       judge,
		errorLast:   errAcc,
		maxAbsDelta: maxAbsDelta,
	}
}

// recover extracts the support-vector index sets and rebuilds w and b.
func (m *model) recover(x [][D]float64, y []float64) {
	n := len(y)

	sMargin := make([]int, 0, n)
	sInside := make([]int, 0, n)
	for i := 0; i < n; i++ {
		if EpsSV < m.alpha[i] && m.alpha[i] < C-EpsSV {
			sMargin = append(sMargin, i)
		}
	}
	for i := 0; i < n; i++ {
		if m.alpha[i] >= C-EpsSV {
			sInside = append(sInside, i)
		}
	}
	m.sMargin = sMargin
	m.sInside = sInside
	m.nsMargin = len(sMargin)
	m.nsInside = len(sInside)

	for d := 0; d < D; d++ {
		m.w[d] = 0.0
	}
	for d := 0; d < D; d++ {
		for _, i := range sMargin {
			m.w[d] = m.w[d] + m.alpha[i]*y[i]*x[i][d]
		}
		for _, i := range sInside {
			m.w[d] = m.w[d] + m.alpha[i]*y[i]*x[i][d]
		}
	}

	b := 0.0
	for _, i := range sMargin {
		dp := 0.0
		for d := 0; d < D; d++ {
			dp = dp + m.w[d]*x[i][d]
		}
		b = b + (y[i] - dp)
	}
	if m.nsMargin == 0 {
		fmt.Fprintln(os.Stderr, "FAIL: |S_margin| == 0")
		os.Exit(1)
	}
	m.b = b / float64(m.nsMargin)
}

// f is the decision function.
func (m *model) f(p [D]float64) float64 {
	s := 0.0
	for d := 0; d < D; d++ {
		s = s + m.w[d]*p[d]
	}
	return s + m.b
}

// g is the label predictor.
func (m *model) g(p [D]float64) int {
	if m.f(p) >= 0.0 {
		return 1
	}
	return -1
}

// summarize computes the dual objective and the alpha summaries.
func (m *model) summarize(gm [][]float64, y []float64) {
	n := len(y)

	sum := 0.0
	for i := 0; i < n; i++ {
		sum = sum + m.alpha[i]
	}

	quad := 0.0
	for i := 0; i < n; i++ {
		inner := 0.0
		for j := 0; j < n; j++ {
			inner = inner + m.alpha[i]*m.alpha[j]*y[i]*y[j]*gm[i][j]
		}
		quad = quad + inner
	}

	m.objective = sum - 0.5*quad
	m.alphaSum = sum

	ays := 0.0
	for i := 0; i < n; i++ {
		ays = ays + m.alpha[i]*y[i]
	}
	m.alphaYSum = ays

	cks := 0.0
	for i := 0; i < n; i++ {
		cks = cks + m.alpha[i]*float64((i%97)+1)
	}
	m.alphaChecksm = cks
}

// accuracy counts how many points of a block the model labels as want.
func (m *model) accuracy(block [][D]float64, want int) int {
	correct := 0
	for i := 0; i < len(block); i++ {
		if m.g(block[i]) == want {
			correct++
		}
	}
	return correct
}

// ===========================================================================
// module: app
// ===========================================================================

func main() {
	out := bufio.NewWriter(os.Stdout)
	defer out.Flush()

	ds := buildDataset()
	gm := gram(ds.xTrain)
	m := train(gm, ds.yTrain)
	m.recover(ds.xTrain, ds.yTrain)
	m.summarize(gm, ds.yTrain)

	converged := 1
	if m.judge {
		converged = 0
	}

	// Training accuracy over the 200 training points, class +1 block then
	// class -1 block (which is exactly index order).
	trainCorrect := 0
	for i := 0; i < NTrain; i++ {
		want := 1
		if ds.yTrain[i] < 0.0 {
			want = -1
		}
		if m.g(ds.xTrain[i]) == want {
			trainCorrect++
		}
	}
	trainAcc := float64(trainCorrect) / float64(NTrain)

	correctC1 := m.accuracy(ds.xTest1, 1)
	correctC2 := m.accuracy(ds.xTest2, -1)
	testCorrect := correctC1 + correctC2
	testAcc := float64(testCorrect) / float64(NTest)
	testAccC1 := float64(correctC1) / float64(NTestC1)
	testAccC2 := float64(correctC2) / float64(NTestC2)

	fmt.Fprintf(out, "SVM_VERSION %d\n", SVMVersion)
	fmt.Fprintf(out, "SWEEPS %d\n", Sweeps)
	fmt.Fprintf(out, "CONVERGED %d\n", converged)
	fmt.Fprintf(out, "ERROR_LAST %.6f\n", m.errorLast)
	fmt.Fprintf(out, "MAX_ABS_DELTA %.6f\n", m.maxAbsDelta)
	fmt.Fprintf(out, "BETA %.6f\n", m.beta)
	fmt.Fprintf(out, "NS_MARGIN %d\n", m.nsMargin)
	fmt.Fprintf(out, "NS_INSIDE %d\n", m.nsInside)
	fmt.Fprintf(out, "W %.6f %.6f %.6f %.6f\n", m.w[0], m.w[1], m.w[2], m.w[3])
	fmt.Fprintf(out, "B %.6f\n", m.b)
	fmt.Fprintf(out, "OBJECTIVE %.6f\n", m.objective)
	fmt.Fprintf(out, "ALPHA_SUM %.6f\n", m.alphaSum)
	fmt.Fprintf(out, "ALPHA_Y_SUM %.12f\n", m.alphaYSum)
	fmt.Fprintf(out, "ALPHA_CHECKSUM %.6f\n", m.alphaChecksm)
	fmt.Fprintf(out, "TRAIN_ACC %.6f\n", trainAcc)
	fmt.Fprintf(out, "TEST_ACC %.6f\n", testAcc)
	fmt.Fprintf(out, "TEST_ACC_C1 %.6f\n", testAccC1)
	fmt.Fprintf(out, "TEST_ACC_C2 %.6f\n", testAccC2)
	fmt.Fprintf(out, "TEST_CORRECT %d %d %d\n", correctC1, correctC2, testCorrect)

	out.WriteString("PRED")
	for i := 0; i < NTestC1; i++ {
		fmt.Fprintf(out, " %d", m.g(ds.xTest1[i]))
	}
	for i := 0; i < NTestC2; i++ {
		fmt.Fprintf(out, " %d", m.g(ds.xTest2[i]))
	}
	out.WriteString("\n")

	out.WriteString("ALPHA")
	for i := 0; i < NTrain; i++ {
		fmt.Fprintf(out, " %.6f", m.alpha[i])
	}
	out.WriteString("\n")
}
