#define Uses_THardwareInfo
#include <tvision/tv.h>

#include <internal/buffdisp.h>
#include <internal/codepage.h>
#include <internal/getenv.h>
#include <internal/cursor.h>
#include <chrono>
using std::chrono::microseconds;
using std::chrono::steady_clock;

#ifdef _MSC_VER
#define __builtin_expect(x, y) x
#endif

BufferedDisplay *BufferedDisplay::instance = 0;

BufferedDisplay::BufferedDisplay() :
    // This could be checked at runtime, but for now this is as much as I know.
    widePlaceholder('\0') // Replace with space if terminal treats all characters one column wide.
#ifdef _WIN32
    , wideOverlapping(false)
#else
    , wideOverlapping(true)
#endif
    , flushDelay()
    , lastFlush()
{
    instance = this;
    // Check if FPS shall be limited.
    int fps = getEnv<int>("TVISION_MAX_FPS", defaultFPS);
    limitFPS = (fps > 0);
    if (limitFPS)
        flushDelay = microseconds((int) 1e6/fps);
    maxFrameDrops = max(0, getEnv<int>("TVISION_MAX_FRAMEDROPS", defaultMaxFrameDrops));
    frameDrops = 0;
    // Initialize variables.
    screenChanged = true;
    caretMoved = false;
    caretPosition = {-1, -1};
    newCaretSize = 0;
}

BufferedDisplay::~BufferedDisplay()
{
    instance = 0;
}

void BufferedDisplay::reloadScreenInfo()
{
    DisplayStrategy::reloadScreenInfo();
    resizeBuffer();
}

void BufferedDisplay::resizeBuffer()
{
    buffer.resize(0);
    buffer.resize(size.x*size.y); // Zero-initialized.

    rowDamage.resize(0);
    rowDamage.resize(size.y, {INT_MAX, INT_MIN});
}

void BufferedDisplay::setCaretSize(int size)
{
    newCaretSize = size;
}

void BufferedDisplay::setCaretPosition(int x, int y)
{
    caretPosition = {x, y};
    caretMoved = true;
}

void BufferedDisplay::screenWrite(int x, int y, TScreenCell *buf, int len)
{
    if (inBounds(x, y))
    {
        if (x + len >= size.x)
            len = size.x - x;

        auto changed = screenChanged;
        auto damage = rowDamage[y];
        auto *bufCell = &buffer[y*size.x + x];
        for (int i = 0; i < len; ++i, ++x, ++bufCell)
        {
            BufferCell newCell {buf[i]};
            ensurePrintable(newCell);
            if (newCell != *bufCell)
            {
                changed = true;
                setDirty(x, newCell, damage);
                *bufCell = newCell;
            }
        }
        screenChanged = changed;
        rowDamage[y] = damage;
    }
}

void BufferedDisplay::setDirty(int x, BufferCell &cell, Range &damage)
{
    cell.dirty = 1;
    Range dam = damage;
    if (x < dam.begin)
        dam.begin = x;
    if (x > dam.end)
        dam.end = x;
    damage = dam;
}

bool BufferedDisplay::timeToFlush()
{
    // Avoid flushing faster than the maximum FPS.
    if (limitFPS)
    {
        auto now = steady_clock::now();
        if (frameDrops == maxFrameDrops || ((now - lastFlush) >= flushDelay))
        {
            lastFlush = now;
            frameDrops = 0;
        }
        else
        {
            ++frameDrops;
            return false;
        }
    }
    return true;
}

void BufferedDisplay::drawCursors()
{
    for (auto* cursor : cursors)
        if (cursor->isVisible())
        {
            auto pos = cursor->getPos();
            auto x = pos.x, y = pos.y;
            if (inBounds(x, y))
            {
                auto *cell = &buffer[y*size.x + x];
                if ( cell->Char == TScreenCell::wideCharTrail &&
                     x > 0 && (cell - 1)->extraWidth )
                    --cell, --x;
                cursor->apply(cell->Attr);
                setDirty(x, *cell, rowDamage[y]);
            }
        }
}

void BufferedDisplay::undrawCursors()
{
    for (const auto* cursor : cursors)
        if (cursor->isVisible())
        {
            auto pos = cursor->getPos();
            auto x = pos.x, y = pos.y;
            if (inBounds(x, y))
            {
                auto *cell = &buffer[y*size.x + x];
                if ( cell->Char == TScreenCell::wideCharTrail &&
                     x > 0 && (cell - 1)->extraWidth )
                    --cell, --x;
                cursor->restore(cell->Attr);
                setDirty(x, *cell, rowDamage[y]);
            }
        }
}

bool BufferedDisplay::needsFlush() const
{
    return screenChanged || caretMoved || caretSize != newCaretSize;
}

void BufferedDisplay::flushScreen()
{
    if (needsFlush() && timeToFlush())
    {
        drawCursors();
        FlushScreenAlgorithm(*this).run();
        if (caretPosition.x != -1)
            lowlevelMoveCursor(caretPosition.x, caretPosition.y);
        undrawCursors();
        if (caretSize != newCaretSize)
            lowlevelCursorSize(newCaretSize);
        lowlevelFlush();
        screenChanged = false;
        caretMoved = false;
        caretSize = newCaretSize;
    }
}

void BufferedDisplay::ensurePrintable(BufferCell &cell) const
{
    auto &ch = cell.Char;
    if (!ch[1]) /* size 1 */ {
        auto &c = ch[0];
        if (c == '\0')
            c = ' ';
        else if (c < ' ' || 0x7F <= c)
            // Translate from codepage as fallback.
            ch = CpTranslator::toUtf8Int(c);
    } else if (ch == TScreenCell::wideCharTrail) {
        ch = widePlaceholder;
        cell.extraWidth = 0;
    }
}

static inline bool isNull(BufferCell cell)
{
    return __builtin_expect(cell.Char == '\0', 0);
}

static inline bool isWide(BufferCell cell)
{
    return __builtin_expect(cell.extraWidth, 0);
}

void FlushScreenAlgorithm::run()
{
    size = disp.size;
    last = {INT_MIN, INT_MIN};
    for (y = 0; y < size.y; ++y)
    {
        damage = disp.rowDamage[y];
        if (damage.begin <= damage.end)
        {
            x = damage.begin;
            // Check for a wide character before the first draw position
            // if overlapping is not allowed.
            bool wideSpillBefore = wideCanSpill() && !wideCanOverlap() && 0 < x && isWide(cellAt(y, x-1));
            for (; x <= damage.end; ++x)
            {
                getCell();
                if (cell.dirty || isNull(cell)) {
                    if (wideSpillBefore) {
                        --x;
                        getCell();
                        wideSpillBefore = false;
                    }
                    pCell->dirty = 0;
                    processCell();
                } else
                    wideSpillBefore = wideCanSpill() && !wideCanOverlap() && isWide(cell);
            }
            if (x < size.x) {
                getCell();
                if (isNull(cell))
                    // The beginning of a wide character has been overwritten
                    // and attribute spill may happen in the remaining trails.
                    handleNull();
            }
        }
        disp.rowDamage[y] = {INT_MAX, INT_MIN};
    }
}

void FlushScreenAlgorithm::processCell()
{
    if (wideCanSpill()) {
        if (isWide(cell)) {
            handleWideCharSpill();
            return;
        } else if (isNull(cell)) {
            handleNull();
            return;
        }
    }
    writeCell();
}

void FlushScreenAlgorithm::writeCell()
{
    // 'last' is the last written cell occupied by text.
    // That is, the hardware cursor is located at {last.x + 1, last.y}.

    // Workaround for Ncurses bug
//     if (y != last.y)
//         lowlevelFlush();
    if (y != last.y)
        disp.lowlevelMoveCursor(x, y);
    else if (x != last.x + 1)
        disp.lowlevelMoveCursorX(x, y);

    disp.lowlevelWriteChars(cell.Char.asText(), cell.Attr);
    last = {x + cell.extraWidth, y};
}

void FlushScreenAlgorithm::handleWideCharSpill()
{
    uchar width = cell.extraWidth;
    const auto Attr = cell.Attr;
    if (x + width < size.x)
        writeCell();
    else {
        // Replace with spaces if it would otherwise be printed on the next line.
        cell.Char = ' ';
        writeCell();
        while (--width && ++x < size.x) {
            getCell();
            if (cell.Char != '\0') {
                --x;
                return;
            }
            cell.Char = ' ';
            writeCell();
        }
        return;
    }
    // Ensure character does not spill on next cells.
    int wbegin = x;
    while (width-- && ++x < size.x) {
        getCell();
        pCell->dirty = 0;
        if (cell.Char != '\0') {
            if (wideCanOverlap()) {
                // Write over the wide character.
                writeCell();
                return;
            } else {
                // Replace the whole wide character with spaces.
                int wend = x;
                x = wbegin;
                do {
                    getCell();
                    cell.Char = ' ';
                    writeCell();
                } while (++x < wend);
                getCell();
                writeCell();
                return;
            }
        }
    }
    // If the next character has a different attribute, print it anyway
    // to avoid attribute spill.
    if (x + 1 < size.x) {
        ++x;
        getCell();
        if (Attr != cell.Attr) {
            pCell->dirty = 0;
            processCell();
        } else
            --x;
    }
}

void FlushScreenAlgorithm::handleNull()
{
    // Having '\0' in a cell implies wide characters can spill, as '\0'
    // is otherwise discarded in ensurePrintable().
    const auto Attr = cell.Attr;
    if (x > 0) {
        --x;
        getCell();
        // Check the character behind the placeholder.
        if (cell.extraWidth) {
            handleWideCharSpill();
            return;
        }
        ++x;
        getCell();
    }
    // Print successive placeholders as spaces.
    do {
        pCell->dirty = 0;
        cell.Char = ' ';
        writeCell();
        ++x;
        if (x < size.x)
            getCell();
        else
            return;
    } while (cell.Char == '\0');
    // We now got a normal character.
    if (x > damage.end && Attr != cell.Attr) {
        // Redraw a character that would otherwise not be printed,
        // to prevent attribute spill.
        processCell();
    } else {
        // Decrease for next iteration
        --x;
    }
}

