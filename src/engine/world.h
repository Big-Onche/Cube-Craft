
enum                            // hardcoded texture numbers
{
    DEFAULT_SKY = 0,
    DEFAULT_GEOM,
    NUMDEFAULTSLOTS
};

#define MAPVERSION 2

// Raw lightweight map header. The octree follows immediately; all world
// settings and data-driven cube definitions live in the external map config.
struct mapheader
{
    char magic[4];              // "TMAP"
    int version;                // any >8bit quantity is little endian
    int worldsize;
};

#define WATER_AMPLITUDE 0.4f
#define WATER_OFFSET 1.1f

enum
{
    MATSURF_NOT_VISIBLE = 0,
    MATSURF_VISIBLE,
    MATSURF_EDIT_ONLY
};

#define TEX_SCALE 16.0f

struct vertex { vec pos; bvec4 norm; vec tc; bvec4 tangent; };
