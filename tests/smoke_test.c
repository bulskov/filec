/* Scaffold smoke test: the public headers compile under the project's warning
 * flags, the library links, and the generated version header exists.
 * Replace with real tests as the implementation lands. */

#include "ctt.h"
#include "filec/fs.h"
#include "filec/path.h"
#include "filec/version.h"

TEST(version_header_is_generated)
{
    ASSERT_TRUE(sizeof(FILEC_VERSION_STRING) > 1);
}

int main(int argc, char *argv[])
{
    return ctt_main(argc, argv, "filec smoke");
}
