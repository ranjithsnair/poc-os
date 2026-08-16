#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

struct stat {
  short type;  // Type of file
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short nlink; // Number of links to file
  uint size;   // Size of file in bytes
  ushort mode; // Permission bits (see include/fs.h's struct dinode)
  ushort uid;  // Owner user ID
  ushort gid;  // Owner group ID
};
