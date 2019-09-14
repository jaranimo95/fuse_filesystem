/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/


// Christian Jarani
// CS 1550
// Project 4


#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

// FAT Stuff
#define FAT_SIZE 1
#define START_FAT 10240 - FAT_SIZE
#define FAT_LENGTH (BLOCK_SIZE * FAT_SIZE) / sizeof(short)

// Path Classification Macros
#define PATH_ROOT 0
#define PATH_DIR  1
#define PATH_SUB  2
#define PATH_FILE 3

//The attribute packed means to not align these things
struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];					//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];					//extension (plus space for nul)
		size_t fsize;									//file size
		long nStartBlock;								//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ; typedef struct cs1550_directory_entry cs1550_directory_entry;

#define MAX_DIRS_IN_ROOT (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + sizeof(long))

struct cs1550_root_directory
{
	int nDirectories;	//How many subdirectories are in the root
						//Needs to be less than MAX_DIRS_IN_ROOT
	struct cs1550_directory
	{
		char dname[MAX_FILENAME + 1];	//directory name (plus space for nul)
		long nStartBlock;				//where the directory block is on disk
	} __attribute__((packed)) directories[MAX_DIRS_IN_ROOT];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_DIRS_IN_ROOT * sizeof(struct cs1550_directory) - sizeof(int)];
} ; typedef struct cs1550_root_directory cs1550_root_directory;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

#define START_FILES 1 + MAX_DIRS_IN_ROOT

struct cs1550_disk_block
{
	//The next disk block, if needed. This is the next pointer in the linked 
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
}; typedef struct cs1550_disk_block cs1550_disk_block;

#define MAX_FAT_ENTRIES (BLOCK_SIZE/sizeof(short))              // Define maximum number of entries in our FAT table

struct cs1550_file_alloc_table_block {                          // Define FAT block as a struct
    short table[MAX_FAT_ENTRIES];
}; typedef struct cs1550_file_alloc_table_block cs1550_fat_block;


// Loads root into a struct
static cs1550_root_directory* load_root()
{
    FILE* file = fopen(".disk", "rb");							// Open disk
    void* root = malloc(sizeof(cs1550_root_directory));			// Allocate space for our root
    if(file)													// If disk opened successfully
    {
        fread(root, sizeof(cs1550_root_directory), 1, file);		// Load root from disk
        fclose(file);												// Close disk
    }
    return (cs1550_root_directory*) root;						// Return root
}

// Saves the root struct to the disk
static void save_root(cs1550_root_directory* root)
{
    FILE* file = fopen(".disk", "r+b");							// Open disk
    fwrite(root, 1, sizeof(cs1550_root_directory), file);		// Write root to disk
    fclose(file);												// Close disk
}

// Finds a file in a directory
static int find_file(cs1550_directory_entry* dir, char* filename, char* extension)
{
    int i;
    for(i = 0; i < dir->nFiles; i++) {	// For each file in the directory
        if(strcmp(dir->files[i].fname, filename) == 0 && strcmp(dir->files[i].fext, extension) == 0) // If file exists
            return i;	                // Return where file lives
    }
    return -1;	                        // Else file doesn't exist
}

// Finds a directory from the root
static int find_dir(cs1550_root_directory* root, char* directory)
{
    int i;
    for(i = 0; i < root->nDirectories; i++) {                   // Iterate through all directories
        if(strcmp(root->directories[i].dname, directory) == 0)  // If the directory exists
            return i;                                               // Return where it lives
    }
    return -1;                                                  // Else can't find it
}

// Loads the specificed directory into a struct
static cs1550_directory_entry* load_dir(long nStartBlock)
{
    void* dir = malloc(sizeof(cs1550_directory_entry));			// Allocate space for directory
    FILE* file = fopen(".disk", "rb");							// Open disk
    if(file)													// If disk opened successfully
    {
        fseek(file, nStartBlock * BLOCK_SIZE, SEEK_SET);			// Seek to block where directory lives
        fread(dir, sizeof(cs1550_directory_entry), 1, file);		// Load directory from disk
        fclose(file);												// Close disk
    }
    return (cs1550_directory_entry*) dir;						// Return directory
}

// Saves a directory entry to the disk
static void save_dir(cs1550_directory_entry* dir, long nStartBlock)
{
    FILE* file = fopen(".disk", "r+b");                         // Open disk
    fseek(file, nStartBlock * BLOCK_SIZE, SEEK_SET);            // Seek to disk location to save our directory to
    fwrite(dir, 1, sizeof(cs1550_directory_entry), file);       // Write directory to disk
    fclose(file);                                               // Close disk
}

// Saves a block to the disk
static void save_block(cs1550_disk_block* block, long nStartBlock)
{
    FILE* file = fopen(".disk", "r+b");                         // Open disk
    fseek(file, nStartBlock * BLOCK_SIZE, SEEK_SET);            // Seek to disk location to save our block to
    fwrite(block, 1, sizeof(cs1550_disk_block), file);          // Write block to disk
    fclose(file);                                               // Close disk
}

// Loads the block from the disk
static cs1550_disk_block* load_block(long nStartBlock)
{
    void* block = malloc(sizeof(cs1550_disk_block));            // Allocate space in memory to hold our block
    FILE* file = fopen(".disk", "rb");                          // Open disk
    if(file)
    {
        fseek(file, nStartBlock * BLOCK_SIZE, SEEK_SET);            // Seek to disk location we saved our block to
        fread(block, sizeof(cs1550_disk_block), 1, file);           // Read block from disk
        fclose(file);                                               // Close disk
    }
    return (cs1550_disk_block*) block;
}

// Loads fat from disk
static short* load_fat()
{
    void* fat = malloc(sizeof(short) * FAT_LENGTH);         // Allocate space in memory for our fat
    FILE* file = fopen(".disk", "rb");                      // Open disk
    if(file)
    {
        fseek(file, START_FAT * BLOCK_SIZE, SEEK_SET);          // Seek to disk location holding our fat
        fread(fat, FAT_SIZE * BLOCK_SIZE, 1, file);             // Read fat from disk
        fclose(file);                                           // Close disk
    }

    return (short*) fat;
}

// Finds the next free entry in our fat
static int find_empty_fat_index(short* fat)
{
    int i;
    for(i = 0; i < FAT_LENGTH; i++)             // For all entries in our fat
    {
        if(fat[i] == 0) return i;               // If entry is empty, return index of empty entry
    }
    return -1;  // Else if no empty index, return error

}

// Saves the fat to the disk
static void save_fat(short* fat)
{
    FILE* file = fopen(".disk", "r+b");                 // Open disk
    fseek(file, START_FAT * BLOCK_SIZE, SEEK_SET);      // Seek to starting block we've dedicated to our fat
    fwrite(fat, 1, BLOCK_SIZE * FAT_SIZE, file);        // Write fat to disk
    fclose(file);                                       // Close disk
}

// Splits up path into each of it's separate components (directory, filename, extension)
static int parse_path(const char* path, char* directory, char* filename, char* extension)
{
    directory[0] = 0;
    filename[0] = 0;
    extension[0] = 0;

    sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

    if(directory[0] == 0) 	// If no directory
        return PATH_ROOT;
    if(filename[0] == 0) 	// If no filename
        return PATH_DIR;
    if(extension[0] == 0) 	// If no extension
        return PATH_SUB;
    return PATH_FILE; 		// All empty
}

/*
 * Called whenever the system wants to know the file attributes, including
 * simply whether the file exists or not. 
 *
 * man -s 2 stat will show the fields of a stat structure
 */
static int cs1550_getattr(const char *path, struct stat *stbuf)
{
	int res = 0;

	memset(stbuf, 0, sizeof(struct stat));
   
	char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);
	
	printf("\nPATH: %s\n",path);

	if (path_type == PATH_ROOT) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	}
	else if (path_type == PATH_DIR) {
		cs1550_root_directory* root = load_root();
        if(find_dir(root, directory) != -1) {
            stbuf->st_mode = S_IFDIR | 0755;
            stbuf->st_nlink = 2;
        }
        else res = -ENOENT;
        free(root);
	}
	else if(path_type == PATH_FILE) {
        cs1550_root_directory* root = load_root();
        cs1550_directory_entry* dir = load_dir(root->directories[find_dir(root, directory)].nStartBlock);
        int fileIndex = find_file(dir, filename, extension);
        if(fileIndex != -1)
        {
            stbuf->st_mode = S_IFREG | 0666;
            stbuf->st_nlink = 1;
            stbuf->st_size = dir->files[fileIndex].fsize;
        }
        else
        {
            res = -ENOENT; //otherwise not found
        }
        free(dir);
        free(root);\
	}
	else res = -ENOENT;

	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	int res = 0;

	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler
	(void) offset;
	(void) fi;

	//the filler function allows us to add entries to the listing
	//read the fuse.h file for a description (in the ../include dir)
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);

	if(path_type == PATH_ROOT)
    {
        cs1550_root_directory* root = load_root();
        int i;
        for(i = 0; i < root->nDirectories; i++)                 // Iterate over all subdirectories
        {
            filler(buf, root->directories[i].dname, NULL, 0);   // Add subdirectory name
        }
        free(root);                                             // Deallocate root directory
	}
    else if(path_type == PATH_DIR)
    {
        cs1550_root_directory* root = load_root();
        cs1550_directory_entry* dir = load_dir(root->directories[find_dir(root, directory)].nStartBlock);
        int i;
        char fullname[MAX_FILENAME + MAX_EXTENSION + 1];
        for(i = 0; i < dir->nFiles; i++)                    // Iterate over all files in directory
        {
            strcpy(fullname, dir->files[i].fname);
            strcat(fullname, ".");
            strcat(fullname, dir->files[i].fext);
            filler(buf, fullname, NULL, 0);                 // Add filename + extension to buffer
        }
        free(root);                                     // Deallocate root directory
    }
    else res = -ENOENT;

    return res;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{	
	int res = 0;
	(void) path;
	(void) mode;

    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);

    if(path_type == PATH_DIR) 						// If path is a directory
    {
        if(strlen(path) > MAX_FILENAME + 1) 		// Filename length check
    		res = -ENAMETOOLONG;

        cs1550_root_directory* root = load_root();	// Load root
        
        int i;
        for(i = 0; i < root->nDirectories; i++) 	// Iterate over all directories
        {
            if(strcmp(root->directories[i].dname, directory) == 0) // If directory is already found
                res = -EEXIST;
            else if(root->directories[i].nStartBlock == 0) 			// Else if found in a valid block
                break;
        }
        strncpy(root->directories[i].dname, directory, MAX_FILENAME + 1); 	// Copy filename into directory
        root->directories[i].nStartBlock = 1 + i; 							// Set location in directory
        root->nDirectories++; 												// Increment number of directories
        save_root(root); 													// Save to disk
        free(root);															// Free up memory used by root
    }
    else if(path_type == PATH_SUB) 	// Else if subdirectory
        res = -EPERM; 				// That ain't allowed

    return res;
}

/* 
 * Removes a directory.
 */
static int cs1550_rmdir(const char *path)
{
	(void) path;
    return 0;
}

/* 
 * Does the actual creation of a file. Mode and dev can be ignored.
 *
 */
static int cs1550_mknod(const char *path, mode_t mode, dev_t dev)
{
	int res = 0;

    (void) mode;
    (void) dev;

    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);

    if(strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION)
    {
        res = -ENAMETOOLONG;
    }
    else if(path_type == PATH_DIR)
    {
        res = -EPERM;
    }
    else
    {
        cs1550_root_directory* root = load_root();                                      // Load root from disk
        int nStartBlock = root->directories[find_dir(root, directory)].nStartBlock;     // Get start block of directory
        cs1550_directory_entry* dir = load_dir(nStartBlock);                            // Load directory
        if(find_file(dir, filename, extension) != -1)                                   // Check if file already exists
        {
            res = -EEXIST;
        }
        else
        {
            short* fat = load_fat();                                            // Load FAT from disk
            int fat_index = find_empty_fat_index(fat);                          // Find empty entry in FAT
            fat[fat_index] = -1;

            strcpy(dir->files[dir->nFiles].fname, filename);                    // Update meta data
            strcpy(dir->files[dir->nFiles].fext, extension);
            dir->files[dir->nFiles].fsize = 0;
            dir->files[dir->nFiles].nStartBlock = fat_index + START_FILES;      // Set new starting point
            dir->nFiles++;

            save_fat(fat);                                                      // Save fat back to disk
            save_dir(dir, nStartBlock);                                         // Save directory back to disk
            free(fat);                                                          // Deallocate FAT
        }
        free(dir);                                                          // Deallocate subdirectory
        free(root);                                                         // Deallocate root directory
    }
    return res;
}

/*
 * Deletes a file
 */
static int cs1550_unlink(const char *path)
{
    (void) path;

    return 0;
}

/* 
 * Read size bytes from file into buf starting from offset
 *
 */
static int cs1550_read(const char *path, char *buf, size_t size, off_t offset,
			  struct fuse_file_info *fi)
{
	(void) buf;
	(void) offset;
	(void) fi;
	(void) path;

    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);

    if(path_type == PATH_DIR) return -EISDIR;   // If path is a directory, return error
    else if(offset > size)    return -1;        // Else if desired offset greater than size of our file, return error
    else if(path_type == PATH_FILE && size > 0) // Else if path is a file with stuff in it
    {
        cs1550_root_directory* root = load_root();                                  // Load root
        int nStartBlock = root->directories[find_dir(root, directory)].nStartBlock; // Find file directory
        cs1550_directory_entry* dir = load_dir(nStartBlock);                        // Load directory
        int fileIndex = find_file(dir, filename, extension);                        // Find file in directory
        int file_nStartBlock = dir->files[fileIndex].nStartBlock;                   // Go to first block of file

        int a = file_nStartBlock;
        int b = START_FILES;
        int fat_index = a - b;          // No clue why this works, but i don't care :)

        size = dir->files[fileIndex].fsize; // Get file size

        cs1550_disk_block* block = load_block(dir->files[fileIndex].nStartBlock);   // Load first block of file

        short* fat = load_fat();                                                    // Load FAT

        int k = fat_index;
        while(offset >= MAX_DATA_IN_BLOCK)      // Move to desired offset
        {
            k = fat[k];
            offset -= MAX_DATA_IN_BLOCK;
        }

        block = load_block(START_FILES + k);    // Load the block (includes the offset)
        strcpy(buf, block->data + offset);      // Start writing the offset data to the buffer

        while(fat[k] != -1) // Write the rest of the data to buffer
        {
            k = fat[k];
            block = load_block(START_FILES + k);
            strcat(buf, block->data);
        }


        free(fat);      // Deallocate FAT
        free(block);    // " block
        free(dir);      // " subdirectory
        free(root);     // " root directory
    }
    else return -1;     // Else return error


	return size;        // Return size
}

/* 
 * Write size bytes from buf into file starting from offset
 *
 */
static int cs1550_write(const char *path, const char *buf, size_t size, 
			  off_t offset, struct fuse_file_info *fi)
{
	(void) buf;
    (void) offset;
    (void) fi;
    (void) path;

    char directory[MAX_FILENAME + 1];
    char filename[MAX_FILENAME + 1];
    char extension[MAX_EXTENSION + 1];
    int path_type = parse_path(path, directory, filename, extension);

    if(path_type == PATH_FILE && size > 0) // If path exists
    {
        if(offset <= size) // If offset within bounds of file
        { 
            // See identical code in cs1550_read for detailed comments
            cs1550_root_directory* root = load_root();
            int nStartBlock = root->directories[find_dir(root, directory)].nStartBlock;
            cs1550_directory_entry* dir = load_dir(nStartBlock);
            int fileIndex = find_file(dir, filename, extension);
            int file_nStartBlock = dir->files[fileIndex].nStartBlock;

            int a = file_nStartBlock;
            int b = START_FILES;
            int fat_index = a - b;          // Still don't know why this works

            short* fat = load_fat();        // Load FAT

            if(offset == size) // If offset at EOF
            {
                while(fat[fat_index] != -1)         // While not at EOF
                {
                    fat_index = fat[fat_index];
                }
                int a = find_empty_fat_index(fat);  // Find next empty FAT entry for new file block
                fat[fat_index] = a;                 // Set old EOF to new block
                fat_index = a;                      // Set fat index to new block so we'll write to it
            }
            else              // Else it isn't at the end of the file
            {
                int reset = fat[fat_index];     // Get ready to nuke the current block
                int temp;
                while(reset != -1)              // While not at EOF
                {
                    temp = fat[reset];              // Save index of next block
                    fat[reset] = 0;                 // Set current block to unused
                    reset = temp;                   // Move to next block
                }
            }

            cs1550_disk_block* block = malloc(sizeof(cs1550_disk_block));   // Allocate space for block
            strncpy(block->data, buf, MAX_DATA_IN_BLOCK);                   // Copy data to write to block
            fat[fat_index] = -1;                                            // Set current block as EOF
            save_block(block, b + fat_index);                               // Save block to disk

            if(size > MAX_DATA_IN_BLOCK)                                    // If writing more data than we can to a single block
            {
                int bytes = MAX_DATA_IN_BLOCK;
                int next = fat_index;
                while(bytes < size)                                             // Write to as many blocks as necessary
                {
                    fat[next] = find_empty_fat_index(fat);
                    memset(block->data, 0, MAX_DATA_IN_BLOCK);                  // Target entire block
                    strncpy(block->data, buf + bytes, MAX_DATA_IN_BLOCK);       // Copy data to block
                    save_block(block, START_FILES + fat[next]);                 // Save block to disk
                    next = fat[next];                                           // Move to next file block
                    fat[next] = -1;                                             // Set current block as EOF
                    bytes += MAX_DATA_IN_BLOCK;                                 // Add block to current size of file in bytes
                }
            }

            dir->files[fileIndex].fsize = size; // Set file size

            save_dir(dir, nStartBlock);         // Save subdirectory to disk

            save_fat(fat);                      // Save FAT to disk

            free(fat);          // Deallocate FAT
            free(block);        // " block
            free(dir);          // " subdirectory
            free(root);         // " root directory
        }
        else return -EFBIG;     // Else requesting to offset outside bounds of file
    }
    else return -1;         // Else path does not exist

    return size;
}

/******************************************************************************
 *
 *  DO NOT MODIFY ANYTHING BELOW THIS LINE
 *
 *****************************************************************************/

/*
 * truncate is called when a new file is created (with a 0 size) or when an
 * existing file is made shorter. We're not handling deleting files or 
 * truncating existing ones, so all we need to do here is to initialize
 * the appropriate directory entry.
 *
 */
static int cs1550_truncate(const char *path, off_t size)
{
	(void) path;
	(void) size;

    return 0;
}


/* 
 * Called when we open a file
 *
 */
static int cs1550_open(const char *path, struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;
    /*
        //if we can't find the desired file, return an error
        return -ENOENT;
    */

    //It's not really necessary for this project to anything in open

    /* We're not going to worry about permissions for this project, but 
	   if we were and we don't have them to the file we should return an error

        return -EACCES;
    */

    return 0; //success!
}

/*
 * Called when close is called on a file descriptor, but because it might
 * have been dup'ed, this isn't a guarantee we won't ever need the file 
 * again. For us, return success simply to avoid the unimplemented error
 * in the debug log.
 */
static int cs1550_flush (const char *path , struct fuse_file_info *fi)
{
	(void) path;
	(void) fi;

	return 0; //success!
}


//register our new functions as the implementations of the syscalls
static struct fuse_operations hello_oper = {
    .getattr	= cs1550_getattr,
    .readdir	= cs1550_readdir,
    .mkdir	= cs1550_mkdir,
	.rmdir = cs1550_rmdir,
    .read	= cs1550_read,
    .write	= cs1550_write,
	.mknod	= cs1550_mknod,
	.unlink = cs1550_unlink,
	.truncate = cs1550_truncate,
	.flush = cs1550_flush,
	.open	= cs1550_open,
};

//Don't change this.
int main(int argc, char *argv[])
{
	return fuse_main(argc, argv, &hello_oper, NULL);
}
