/*
 * The three ionogram cleaning stages, decoded from QIonogram.
 *
 * Constants come from the call site in QIonogram::clear() @0x47d20e:
 *   OBJ_SIZE_HORIZONTAL_DEFAULT = 9, OBJ_SIZE_VERTICAL_DEFAULT = 3,
 *   LIMIT_DEFAULT = 11.0f
 */
#include "../src/iganalytics.h"
#include "../src/igmath.h"

#include <cstdio>
#include <vector>

static int failures = 0;

static void check(const char *what, bool ok)
{
    std::printf("  %-42s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

/* Helper: build row pointers over a flat grid. */
struct Grid {
    int w, h;
    std::vector<std::vector<float> > cells;
    std::vector<float *> rows;
    std::vector<const float *> crows;

    Grid(int w_, int h_, float fill = 0.0f)
        : w(w_), h(h_), cells(w_, std::vector<float>(h_, fill)),
          rows(w_), crows(w_)
    {
        rebind();
    }
    void rebind()
    {
        for (int i = 0; i < w; ++i) { rows[i] = cells[i].data(); crows[i] = cells[i].data(); }
    }
    int countPositive() const
    {
        int n = 0;
        for (int x = 0; x < w; ++x)
            for (int y = 0; y < h; ++y)
                if (cells[x][y] > 0.0f) ++n;
        return n;
    }
};

static void test_fill_mask()
{
    std::printf("fillMask:\n");
    Grid src(3, 2), mask(3, 2), dst(3, 2);
    for (int x = 0; x < 3; ++x)
        for (int y = 0; y < 2; ++y)
            src.cells[x][y] = (float)(10 * x + y + 1);
    mask.cells[0][0] = 1.0f;      /* keep */
    mask.cells[1][1] = 0.5f;      /* keep -- any positive value */
    mask.cells[2][0] = 0.0f;      /* drop */
    mask.cells[2][1] = -1.0f;     /* drop -- not > 0 */
    src.rebind(); mask.rebind(); dst.rebind();

    fillMask(dst.rows.data(), src.crows.data(), mask.crows.data(), 3, 2);

    check("keeps src where mask > 0",  dst.cells[0][0] == src.cells[0][0] &&
                                       dst.cells[1][1] == src.cells[1][1]);
    check("zeroes where mask == 0",    dst.cells[2][0] == 0.0f);
    check("zeroes where mask < 0",     dst.cells[2][1] == 0.0f);
    check("zeroes where mask unset",   dst.cells[0][1] == 0.0f);
}

static void test_median_equalize()
{
    std::printf("medianEqualize:\n");
    Grid src(2, 3), dst(2, 3);
    for (int y = 0; y < 3; ++y) { src.cells[0][y] = 4.0f; src.cells[1][y] = 9.0f; }
    src.rebind(); dst.rebind();

    const double medians[2] = { 2.0, 3.0 };
    medianEqualize(dst.rows.data(), src.crows.data(), 2, 3, medians);

    check("divides each spectrum by its median",
          dst.cells[0][0] == 2.0f && dst.cells[1][0] == 3.0f);

    /* A zero median must leave the spectrum alone rather than divide by it. */
    Grid dst2(2, 3, -1.0f); dst2.rebind();
    const double zero[2] = { 0.0, 3.0 };
    medianEqualize(dst2.rows.data(), src.crows.data(), 2, 3, zero);
    check("skips a zero median", dst2.cells[0][0] == -1.0f && dst2.cells[1][0] == 3.0f);
}

static void test_delete_small_objects()
{
    std::printf("deleteSmallObjects (9x3 window, level 11):\n");

    /* A grid big enough that the interior band is meaningful. */
    const int W = 40, H = 20;

    /* An isolated point has one neighbour -- itself -- so it must go. */
    {
        Grid src(W, H), dst(W, H);
        src.cells[20][10] = 5.0f;
        src.rebind(); dst.rebind();
        deleteSmallObjects(dst.rows.data(), src.crows.data(), W, H);
        check("removes an isolated point", dst.countPositive() == 0);
    }

    /* A long horizontal run is exactly the shape a trace has, and the 9-wide
     * window is chosen for it: interior cells see 9 neighbours... still under
     * 11, so a single-row trace alone does not survive. */
    {
        Grid src(W, H), dst(W, H);
        for (int x = 5; x < 35; ++x) src.cells[x][10] = 5.0f;
        src.rebind(); dst.rebind();
        deleteSmallObjects(dst.rows.data(), src.crows.data(), W, H);
        check("a 1-row line is below the threshold", dst.countPositive() == 0);
    }

    /* Three stacked rows give interior cells 27 neighbours, well over 11. */
    {
        Grid src(W, H), dst(W, H);
        for (int x = 5; x < 35; ++x)
            for (int y = 9; y <= 11; ++y)
                src.cells[x][y] = 5.0f;
        src.rebind(); dst.rebind();
        deleteSmallObjects(dst.rows.data(), src.crows.data(), W, H);
        const int kept = dst.countPositive();
        std::printf("  %-42s %d of %d kept\n", "a 3-row band survives", kept, 30 * 3);
        check("a 3-row band mostly survives", kept > 50);
    }

    /* Dense blob plus isolated speckle: the blob stays, the speckle goes. */
    {
        Grid src(W, H), dst(W, H);
        for (int x = 10; x < 30; ++x)
            for (int y = 8; y <= 12; ++y)
                src.cells[x][y] = 5.0f;
        src.cells[3][3] = 9.0f;
        src.cells[36][17] = 9.0f;
        src.rebind(); dst.rebind();
        deleteSmallObjects(dst.rows.data(), src.crows.data(), W, H);
        check("speckle beside a blob is removed",
              dst.cells[3][3] == 0.0f && dst.cells[36][17] == 0.0f);
        check("the blob core is kept", dst.cells[20][10] == 5.0f);
    }
}

int main()
{
    test_fill_mask();
    test_median_equalize();
    test_delete_small_objects();
    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
