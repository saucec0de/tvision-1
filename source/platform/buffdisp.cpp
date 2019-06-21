#include <internal/buffdisp.h>
#include <internal/getenv.h>
#include <chrono>
#include <utility>
using std::pair;
using std::chrono::microseconds;
using std::chrono::steady_clock;
using std::chrono::time_point;

void BufferedDisplay::initBuffer()
{
    int fps = getEnv<int>("TVISION_MAX_FPS", defaultFPS);
    limitFPS = (fps > 0);
    if (limitFPS)
        flushDelay = microseconds((int) 1e6/fps);
    // Initialize variables.
    lastX = lastY = -1;
    needsFlush = true;
    cursorMoved = false;
    // Allocate buffer.
    int rows = getScreenRows(), cols = getScreenCols();
    charBuffer = Array2D<uchar>(rows, cols, 0);
    attrBuffer = Array2D<ushort>(rows, cols, 0);
}

void BufferedDisplay::setCaretPosition(int x, int y)
{
    lastX = x; lastY = y;
    cursorMoved = true;
}

void BufferedDisplay::screenWrite( int x, int y, ushort *buf, int len )
{
    for (int i = 0; i < len; i++, x++)
    {
        uchar character = buf[i*2];
        ushort attr = buf[i*2 + 1];
        if (character != charBuffer[y][x] || attr != attrBuffer[y][x])
        {
            needsFlush = true;
            changes.insert({y, x});
            charBuffer[y][x] = character;
            attrBuffer[y][x] = attr;
        }
    }
}

bool BufferedDisplay::timeToFlush()
{
    // Avoid flushing faster than the maximum FPS.
    bool b = true;
    if (limitFPS)
    {
        auto now = steady_clock::now();
        if ((b = ((now - lastFlush) >= flushDelay)))
            lastFlush = now;
    }
    return b;
}

void BufferedDisplay::flushScreen()
{
    if ((needsFlush || cursorMoved) && timeToFlush())
    {
        struct { int x, y; } last = {-1, -1};
        for (const pair<int, int> &pos : changes)
        {
            int x = pos.second, y = pos.first;
            if (y != last.y || x != last.x + 1)
                lowlevelMoveCursor(x, y);
            lowlevelWriteChar(charBuffer[y][x], attrBuffer[y][x]);
            last = {x, y};
        }
        if (lastX != -1)
            lowlevelMoveCursor(lastX, lastY);
        lowlevelFlush();
        cursorMoved = false;
        needsFlush = false;
        changes.clear();
    }
}

