version = "0.0.0"
major = 0
minor = 0
patch = 0
version_commit = 0
commit = "uwp-local-games"
date = "UWP custom build"

print(f'#define XEMU_VERSION       "{version}"')
print(f'#define XEMU_VERSION_MAJOR {major}')
print(f'#define XEMU_VERSION_MINOR {minor}')
print(f'#define XEMU_VERSION_PATCH {patch}')
print(f'#define XEMU_VERSION_COMMIT {version_commit}')
print(f'#define XEMU_COMMIT        "{commit}"')
print(f'#define XEMU_DATE          "{date}"')
