/*
	FUSE: Filesystem in Userspace
	Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

	This program can be distributed under the terms of the GNU GPL.
	See the file COPYING.
*/

#define	FUSE_USE_VERSION 26

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>

//size of a disk block
#define	BLOCK_SIZE 512

//we'll use 8.3 filenames
#define	MAX_FILENAME 8
#define	MAX_EXTENSION 3

//How many files can there be in one directory?
#define MAX_FILES_IN_DIR (BLOCK_SIZE - sizeof(int)) / ((MAX_FILENAME + 1) + (MAX_EXTENSION + 1) + sizeof(size_t) + sizeof(long))

//The attribute packed means to not align these things

struct cs1550_directory_entry
{
	int nFiles;	//How many files are in this directory.
				//Needs to be less than MAX_FILES_IN_DIR

	struct cs1550_file_directory
	{
		char fname[MAX_FILENAME + 1];	//filename (plus space for nul)
		char fext[MAX_EXTENSION + 1];	//extension (plus space for nul)
		size_t fsize;					//file size
		long nStartBlock;				//where the first block is on disk
	} __attribute__((packed)) files[MAX_FILES_IN_DIR];	//There is an array of these

	//This is some space to get this to be exactly the size of the disk block.
	//Don't use it for anything.  
	char padding[BLOCK_SIZE - MAX_FILES_IN_DIR * sizeof(struct cs1550_file_directory) - sizeof(int)];
} ;

typedef struct cs1550_root_directory cs1550_root_directory;

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
} ;


typedef struct cs1550_directory_entry cs1550_directory_entry;

//How much data can one block hold?
#define	MAX_DATA_IN_BLOCK (BLOCK_SIZE - sizeof(long))

struct cs1550_disk_block
{
	//The next disk block, if needed. This is the next pointer in the linked 
	//allocation list
	long nNextBlock;

	//And all the rest of the space in the block can be used for actual data
	//storage.
	char data[MAX_DATA_IN_BLOCK];
};

typedef struct cs1550_disk_block cs1550_disk_block;

//create a file allocation table that accounts for 1048576(5 mebibytes) / 512(bytes per block) = 2048 blocks of data needed to be accounted for
//we can account for 2048 blocks of data represented in 512 bytes, by making each entry only 2 bits long (infeaseable) or using 4 blocks with char entries. (1 byte entry that represents each block)
struct cs1550_allocation_table{
	//0 is unallocated
	//1 is allocated
	char blocks[2048];
};
typedef struct cs1550_allocation_table cs1550_allocation_table;
//function to read from the root on disk (block 0)
//This returns a copy of the root directory struct for functions to reference
static cs1550_root_directory read_root(){
	cs1550_root_directory root;

	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, 0, SEEK_SET);

	fread(&root, BLOCK_SIZE, 1, disk);

	fclose(disk);
	return root;
}

//function to write a new root struct to the root on disk (block 0)
static void write_root(struct cs1550_root_directory *root){

	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, 0, SEEK_SET);

	fwrite(root, BLOCK_SIZE, 1, disk);

	fclose(disk);
}
//function to read from the file allocation table on disk (block 1-4)
static cs1550_allocation_table read_allTable(){

	cs1550_allocation_table allTable;

	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE, SEEK_SET);

	fread(&allTable, BLOCK_SIZE*4, 1, disk);

	fclose(disk);
	return allTable;
}
//function to write to the file allocation table on disk (block 1-4)
static void write_allTable(struct cs1550_allocation_table *allTable){


	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE, SEEK_SET);

	fwrite(allTable, BLOCK_SIZE*4, 1, disk);

	fclose(disk);
}
//function to read from a directory on disk
static cs1550_directory_entry read_dirEntry(long blockNum){


	cs1550_directory_entry dirEntry;

	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE*blockNum, SEEK_SET);

	fread(&dirEntry, BLOCK_SIZE, 1, disk);

	fclose(disk);
	return dirEntry;
}
//function to place a new directory in disk
static void write_dirEntry(struct cs1550_directory_entry *dirEntry, long blockNum){


	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, blockNum*BLOCK_SIZE, SEEK_SET);

	fwrite(dirEntry, BLOCK_SIZE, 1, disk);

	fclose(disk);
}
//function to read from a block on disk
static cs1550_disk_block read_block(long blockNum){


	cs1550_disk_block block;

	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, BLOCK_SIZE*blockNum, SEEK_SET);

	fread(&block, BLOCK_SIZE, 1, disk);

	fclose(disk);
	return block;
}
//function to write to a directory in disk
static void write_block(struct cs1550_disk_block *block, long blockNum){


	FILE* disk = fopen(".disk", "r+b");
	fseek(disk, blockNum*BLOCK_SIZE, SEEK_SET);

	fwrite(block, BLOCK_SIZE, 1, disk);

	fclose(disk);
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
	int i, j, dirFound = 0, fileFound = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_file_directory file;
	memset(stbuf, 0, sizeof(struct stat));

	//set the fields for the path to be parsed into in case the path is not the root directory
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));

	struct cs1550_root_directory root;

	//save path into variables
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	
   	if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION){
		return -ENAMETOOLONG;
	}
	//is path the root dir?
	if (strcmp(path, "/") == 0) 
	{
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
		res = 0;
		return res;
	} 
	else 
	{
		//**Check if name is subdirectory**
		
		//read from the directories array in the root block and scan for a directory with the same name
		root = read_root();
	
		for(i=0;i<root.nDirectories;i++)
		{
			//look in the array of directories to find the directory given in the path
			if(strcmp(root.directories[i].dname,directory) == 0)
			{
				
				dir = root.directories[i];
				dirFound = 1;
				break;
			}
		}
		if(dirFound)
		{
			//If filename is blank, the user wanted to return the directory stats
			if(strcmp(filename, "")==0)
			{
				//The filename is a subdirectory
				
				//Might want to return a structure with these fields
				stbuf->st_mode = S_IFDIR | 0755;
				stbuf->st_nlink = 2;
				res = 0; //no error
				

			}
			else
			{
				//If filename is not blank, scan through the given directory and try to find the regular file
				dirEntry = read_dirEntry(dir.nStartBlock);
			
				
				for(j=0;j<dirEntry.nFiles;j++)
				{
					//look in the array of files to see if this file exists
						
					if(strcmp(dirEntry.files[j].fname,filename) == 0 && strcmp(dirEntry.files[j].fext, extension) == 0)
					{
						
						file = dirEntry.files[j];
						fileFound = 1;
						break;
					}
				}
				if(fileFound)
				{
					//regular file matching the filename has been found.
						
						
					//regular file, probably want to be read and write
					stbuf->st_mode = S_IFREG | 0666; 
					stbuf->st_nlink = 1; //file links
					stbuf->st_size = file.fsize; //file size - make sure you replace with real size!
					res = 0; // no error
					
				}
				else
				{
					//The file was not found, 
					//Else return that path doesn't exist
					res = -ENOENT;
				}
			}
			
			
		}
		else
		{
			//The directory was not found, 
			//Else return that path doesn't exist
			res = -ENOENT;
		}

	}
	return res;
}

/* 
 * Called whenever the contents of a directory are desired. Could be from an 'ls'
 * or could even be when a user hits TAB to do autocompletion
 */
static int cs1550_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
			 off_t offset, struct fuse_file_info *fi)
{
	//Since we're building with -Wall (all warnings reported) we need
	//to "use" every parameter, so let's just cast them to void to
	//satisfy the compiler



	(void) offset;
	(void) fi;

	int i, j, dirFound = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_root_directory root;

	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));


	//save path into variables
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//check if any of the path paremeters are malformed
	if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION){
		return -ENAMETOOLONG;
	}

	//check if the filename or extension are blank
	if((!strcmp(filename, "") == 0) || (!strcmp(extension, "") == 0))
	{
		//If either filed is populated, user is trying to list files in a file, which doesnt make any sense
	}

	//Use filler functions to populate the entries into the listig for ls command
	filler(buf, ".", NULL, 0);
	filler(buf, "..", NULL, 0);

	//User wants to list subdirectories of root
	if (strcmp(path, "/") == 0)
	{
		//fill from the root
		root = read_root();
		for(i=0;i<root.nDirectories;i++)
		{
			dir=root.directories[i];
			
			if(strcmp(dir.dname, "") != 0)
			{
				filler(buf, dir.dname, NULL, 0);
			}
			
		}
	}
	else
	{
		//search the root for a directory matching the given directory name
		root = read_root();
		for(i=0;i<root.nDirectories;i++)
		{
			dir=root.directories[i];
			
			if(strcmp(dir.dname, directory) == 0)
			{
				//found the sub directory
				dirFound = 1;
				break;
			}
			
		}
		//directory wasn't found
		if(!dirFound)
		{
			return -ENOENT;
		}
		//directory was found
		else
		{
			dirEntry = read_dirEntry(dir.nStartBlock);
			for(j=0;j<dirEntry.nFiles;j++)
			{
				//print the file name and concatenated extension for each file
				char fileAndExt[(MAX_FILENAME+1)+(MAX_EXTENSION+1)];
				strcpy(fileAndExt, dirEntry.files[j].fname);
				strcat(fileAndExt, ".");
				strcat(fileAndExt, dirEntry.files[j].fext); 
				//print the concatenated file name and extension to filler
				filler(buf, fileAndExt, NULL, 0);
			}
		}
	}


	/*
	//add the user stuff (subdirs or files)
	//the +1 skips the leading '/' on the filenames
	filler(buf, newpath + 1, NULL, 0);
	*/
	return 0;
}

/* 
 * Creates a directory. We can ignore mode since we're not dealing with
 * permissions, as long as getattr returns appropriate ones for us.
 */
static int cs1550_mkdir(const char *path, mode_t mode)
{

	(void) path;
	(void) mode;

	int i, j, blockFound = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_root_directory root;
	struct cs1550_allocation_table fat;

	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));


	//save path into variables
	sscanf(path, "/%[^/]", directory);
	

	//check length
	if(strlen(directory) > MAX_FILENAME){
		return -ENAMETOOLONG;
	}


	//make sure user gave a name for the directory
	if(strcmp(directory,"")==0)
	{
		//directory is populated, refuse to make a directory in a directory other than the root
		return -EINVAL;
	}

	root = read_root();
	
	
	fat = read_allTable();

	if(root.nDirectories >= MAX_DIRS_IN_ROOT)
	{
		//no more directories can be added at this time due to space constraints
		return -EPERM;
	}
	//search through all of the directories to see if one by that name already exists
	for(i = 0; i < root.nDirectories; i++)
	{ 
		if(strcmp(root.directories[i].dname, directory) == 0)
		{
			return -EEXIST;
		}
	}
	//If no directory exists at the passed path, create one using the file allocation table for reference

	
	//find a block to put the new directory using the FAT
	for(j = 6; j<=2048; j++)//iterate through every block in the FAT(not including the root and fat itself which accounts for 5 blocks)
	{
		if(fat.blocks[j] == 0)
		{
			//an empty block has been found to put the directory
			blockFound = 1;
			break;
		}
	}
	if(blockFound)
	{

		//if a free block is found, fill dir struct with info provided by user
		strcpy(dir.dname, directory); 
		dir.nStartBlock = j;
		
		//put a new directory struct at j (the offset into disk that we found)
		memset(&dirEntry, 0, BLOCK_SIZE);
		write_dirEntry(&dirEntry, dir.nStartBlock);

		//update the FAT and say that its now occupied
		fat.blocks[j]=1;
		write_allTable(&fat);

		//add the directory to the root array of directories
		root.directories[root.nDirectories] = dir;

		//update number of directories in root
		root.nDirectories+=1;

		//write out the new root to save changes
		write_root(&root);
	}
	else
	{
		//no room on disk
		return -EPERM;
	}

	return 0;
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
	//should be very similar to mkdir

	(void) mode;
	(void) dev;
	(void) path;

	int i = 0, j = 0, blockFound = 0, dirFound = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_root_directory root;
	struct cs1550_allocation_table fat;
	struct cs1550_file_directory file;

	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));


	//save path into variables
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	//check lengths
	if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION){
		return -ENAMETOOLONG;
	}

	//check if the filename or extension are blank
	if((strcmp(filename, "") == 0) || (strcmp(extension, "") == 0))
	{
		//If either fields are empty, user is trying to make a nameless node
		return -EINVAL;
	}

	//make sure user gave a name for the directory
	if(strcmp(directory,"")==0)
	{
		//directory is blank, refuse to make a file in a blank directory
		return -EINVAL;
	}

	root = read_root();
	fat = read_allTable();

	
	//search through all of the directories to see if the supplied directory exists
	for(i = 0; i < root.nDirectories; i++)
	{ 
		if(strcmp(root.directories[i].dname, directory) == 0)
		{
			dirFound = 1;
			dir = root.directories[i];
			break;
		}
	}

	if(dirFound)
	{

		//do checks on the directory entry
		dirEntry = read_dirEntry(dir.nStartBlock);
		if(dirEntry.nFiles>=MAX_FILES_IN_DIR)
		{
			//no more room in the directory for this file
			return -EPERM;
		}
		//check if the file already exists in the directory

		for(j = 0; j<dirEntry.nFiles; j++)//iterate through every file
		{
			if(strcmp(dirEntry.files[j].fname,filename) == 0 && strcmp(dirEntry.files[j].fext, extension) == 0)
			{
				//this file already exists in the directory, cannot add
				return -EEXIST;
			}
		}

		//check for a free block in the FAT to put the file
		for(j = 6; j<=2048; j++)//iterate through every block in the FAT(not including the root and fat itself which accounts for 5 blocks)
		{
			if(fat.blocks[j] == 0)
			{
				//an empty block has been found to put the start of the file
				blockFound = 1;
				break;
			}
		}
		if(blockFound)
		{

			//fill file information
			strcpy(file.fname, filename);
			strcpy(file.fext, extension); 
			file.fsize = 0;
			file.nStartBlock = j;

			//add file to files array in directory
			dirEntry.files[dirEntry.nFiles] = file;

			//change number of files in directory
			dirEntry.nFiles++;

			//write changes to the dirEntry to make changes permanent
			write_dirEntry(&dirEntry, dir.nStartBlock);

			//update the FAT and say that its now occupied
			fat.blocks[j]=1;

			write_root(&root);

			write_allTable(&fat);

		}
		else
		{
			//no room on disk
			return -EPERM;
		}

	}
	return 0;
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

	int res = 0;
	int i, j, k, dirFound = 0, fileFound = 0;
	int siz  = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_file_directory file;
	struct cs1550_disk_block block;

	long next, currBlock;

	//set the fields for the path to be parsed into in case the path is not the root directory
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];
	char stringBuffer[BLOCK_SIZE];
	
	FILE* disk;

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));

	struct cs1550_root_directory root;

	//save path into variables
	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);
	
	//check that no fields are blank
	if(strcmp(directory, "")==0 || strcmp(filename, "")==0 || strcmp(extension, "")==0)
	{
		return -EEXIST;
	}
	//check lengths
   	if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION)
   	{
		return -ENAMETOOLONG;
	}
	
	//read from the directories array in the root block and scan for a directory with the same name
	root = read_root();

	for(i=0;i<root.nDirectories;i++)
	{
		//look in the array of directories to find the directory given in the path

		if(strcmp(root.directories[i].dname,directory) == 0)
		{
			
			dir = root.directories[i];
			dirFound = 1;
			break;
		}
	}
	if(dirFound)
	{

		dirEntry = read_dirEntry(dir.nStartBlock);

		for(j=0;j<dirEntry.nFiles;j++)
		{
			//look in the array of files to see if this file exists
				
			if(strcmp(dirEntry.files[j].fname,filename) == 0 && strcmp(dirEntry.files[j].fext, extension) == 0)
			{
				
				file = dirEntry.files[j];
				fileFound = 1;
				break;
			}
		}
		if(fileFound)
		{
			//regular file matching the filename has been found.
			//We are ready to start the reading logic

			//make sure file offset is not larger than the file itself
			if(offset>file.fsize)
			{
				return -EFBIG;
			}

			//find which block the offset is located in
			int newOffset, blockNum = offset/BLOCK_SIZE;
			if(blockNum!=0)
			{
				newOffset = offset - blockNum*BLOCK_SIZE;
			}
			else
			{
				newOffset = offset;
			}

			//go to that block

			disk = fopen(".disk", "r+b");
			fseek(disk, file.nStartBlock*BLOCK_SIZE, SEEK_SET);
			fread(&block, BLOCK_SIZE, 1, disk);

			currBlock = file.nStartBlock;

			for(k = 0; k<blockNum; k++)
			{
				next = block.nNextBlock;
				currBlock = next;
				fseek(disk, currBlock*BLOCK_SIZE, SEEK_SET);
				memset(&block, 0, BLOCK_SIZE);
				fread(&block, BLOCK_SIZE, 1, disk);
			}

			//go to the new offset
			fseek(disk, currBlock*BLOCK_SIZE+sizeof(long)+newOffset, SEEK_SET);
			
			memset(stringBuffer, 0, BLOCK_SIZE);

			//read until the end of the block
			fread(stringBuffer, BLOCK_SIZE-(newOffset+sizeof(long)), 1, disk);
			memcpy(buf, stringBuffer, BLOCK_SIZE-(newOffset+sizeof(long)));
			siz += BLOCK_SIZE-(newOffset+sizeof(long));

			memset(stringBuffer, 0, BLOCK_SIZE);

			while(block.nNextBlock!=0)
			{
				//while there are more blocks to be read, read them until end of file

				//navigate to the next block
				next = block.nNextBlock;
				currBlock = next;
				fseek(disk, currBlock*BLOCK_SIZE, SEEK_SET);
				memset(&block, 0, BLOCK_SIZE);
				fread(&block, BLOCK_SIZE, 1, disk);

				
				//copy current block's data content to the buffer

				memcpy(stringBuffer, block.data, BLOCK_SIZE-sizeof(long));
				memcpy(buf+siz, stringBuffer, strlen(stringBuffer));
				siz+=strlen(stringBuffer);
				memset(stringBuffer, 0, BLOCK_SIZE);
			}

			//all blocks have been read onto the buffer, return the size of the buffer and close the file
						
			fclose(disk);
			//send all changes to disk
			write_dirEntry(&dirEntry, dir.nStartBlock);
			write_root(&root);
			
		}
		else
		{
			//The file was not found, 
			//Else return that path doesn't exist
			res = -ENOENT;
		}
	}
	else
	{
		//The directory was not found, 
		//Else return that path doesn't exist
		res = -ENOENT;
	}


	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	return siz;
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

	int res = 0;
	int i, j, k, bytesLeft = strlen(buf), dirFound = 0, fileFound = 0, blockFound = 0, siz = 0;
	struct cs1550_directory dir;
	struct cs1550_directory_entry dirEntry;
	struct cs1550_file_directory file;
	struct cs1550_disk_block block;
	struct cs1550_allocation_table fat;

	long next, currBlock, l;

	//set the fields for the path to be parsed into in case the path is not the root directory
	char directory[MAX_FILENAME+1];
	char filename[MAX_FILENAME+1];
	char extension[MAX_EXTENSION+1];

	FILE* disk;

	memset(directory, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(filename, 0, sizeof(char)*(MAX_FILENAME+1));
	memset(extension, 0, sizeof(char)*(MAX_EXTENSION+1));

	struct cs1550_root_directory root;

	sscanf(path, "/%[^/]/%[^.].%s", directory, filename, extension);

	
	//check that no fields are blank
	if(strcmp(directory, "")==0 || strcmp(filename, "")==0 || strcmp(extension, "")==0)
	{
		return -EINVAL;
	}
	//check lengths
   	if(strlen(directory) > MAX_FILENAME || strlen(filename) > MAX_FILENAME || strlen(extension) > MAX_EXTENSION)
   	{
		return -ENAMETOOLONG;
	}
	

	//read from the directories array in the root block and scan for a directory with the same name
	root = read_root();

	for(i=0;i<root.nDirectories;i++)
	{
		//look in the array of directories to find the directory given in the path

		if(strcmp(root.directories[i].dname,directory) == 0)
		{
			
			dir = root.directories[i];
			dirFound = 1;
			break;
		}
	}
	if(dirFound)
	{

		dirEntry = read_dirEntry(dir.nStartBlock);

		for(j=0;j<dirEntry.nFiles;j++)
		{
			//look in the array of files to see if this file exists
				
			if(strcmp(dirEntry.files[j].fname,filename) == 0 && strcmp(dirEntry.files[j].fext, extension) == 0)
			{
				
				file = dirEntry.files[j];
				fileFound = 1;
				break;
			}
		}
		if(fileFound)
		{
			//regular file matching the filename has been found.
			//We are ready to start the writing logic
			//make sure file offset is not larger than the file itself
			if(offset>file.fsize)
			{
				return -EFBIG;
			}

			//find which block the offset is located in
			int newOffset;
			long blockNum = offset/BLOCK_SIZE;

			if(blockNum!=0)
			{
				newOffset = offset - blockNum*BLOCK_SIZE;
			}
			else
			{
				newOffset = offset;
			}


			//go to that block

			disk = fopen(".disk", "r+b");
			fseek(disk, file.nStartBlock*BLOCK_SIZE, SEEK_SET);
			fread(&block, BLOCK_SIZE, 1, disk);

			currBlock = file.nStartBlock;
			next = currBlock;

			for(k = 0; k<blockNum; k++)
			{
				next = block.nNextBlock;
				fseek(disk, next*BLOCK_SIZE, SEEK_SET);
				memset(&block, 0, BLOCK_SIZE);
				fread(&block, BLOCK_SIZE, 1, disk);
				currBlock = next;
			}

			//go to the new offset
			
			fseek(disk, currBlock*BLOCK_SIZE+sizeof(long)+newOffset, SEEK_SET);
			

			//write the data. When the end of a block is reached, go to the next block until the end of the file is reached.
			//if the data can fit in the starting block, write it and move on
			if(strlen(buf)<=BLOCK_SIZE-(newOffset+sizeof(long)))
			{
				fwrite(buf, strlen(buf), 1, disk);
				bytesLeft-=strlen(buf);
				file.fsize+=strlen(buf);
				siz+=strlen(buf);
			}
			else
			{

				//write until the end of the block
				fwrite(buf, BLOCK_SIZE-(newOffset+sizeof(long)), 1, disk);
				
				bytesLeft-=BLOCK_SIZE-(newOffset+sizeof(long));
				
				file.fsize+=BLOCK_SIZE-(newOffset+sizeof(long));
				
				siz+=BLOCK_SIZE-(newOffset+sizeof(long));

				//If we still have more byes to write, we have two situations.
				while(bytesLeft>0)
				{
					//1. There is a link to the next block in the file
					if(block.nNextBlock!=0)
					{

						next = block.nNextBlock;
						currBlock = next;
						fseek(disk, currBlock*BLOCK_SIZE, SEEK_SET);
						memset(&block, 0, BLOCK_SIZE);
						fread(&block, BLOCK_SIZE, 1, disk);
						fseek(disk, currBlock*BLOCK_SIZE+sizeof(long), SEEK_SET);
						if(bytesLeft>BLOCK_SIZE-sizeof(long))
						{
							fwrite(buf+siz, BLOCK_SIZE-sizeof(long), 1, disk);
							bytesLeft-=(BLOCK_SIZE-sizeof(long));
							file.fsize+=(BLOCK_SIZE-sizeof(long));
							siz+=(BLOCK_SIZE-sizeof(long));
						}
						else
						{
							fwrite(buf+siz, bytesLeft, 1, disk);
							file.fsize+=(bytesLeft);
							size+=(bytesLeft);
							bytesLeft=0;
							break;
						}

					}
					//2. We need to establish a new one
					else
					{
						//find a block in the FAT to expand the file to 
						fat = read_allTable();
						//check for a free block in the FAT to put the file
						for(l = 6; l<=2048; l++)//iterate through every block in the FAT(not including the root and fat itself which accounts for 5 blocks)
						{
							if(fat.blocks[l] == 0)
							{
								//an empty block has been found to put the start of the file
								blockFound = 1;
								break;
							}
						}
						if(blockFound)
						{

							//set the next block pointer to the block found by the fat

							//load current block into block object
							fseek(disk, currBlock*BLOCK_SIZE, SEEK_SET);
							memset(&block, 0, BLOCK_SIZE);
							fread(&block, BLOCK_SIZE, 1, disk);

							//set value of next block to the number found in the fat
							block.nNextBlock = l;							

							//write the changed block back to the disk
							fseek(disk, currBlock*BLOCK_SIZE, SEEK_SET);
							fwrite(&block, BLOCK_SIZE, 1, disk);
							

							//navigate to the new block
							currBlock = l;
							fseek(disk, currBlock*BLOCK_SIZE+sizeof(long), SEEK_SET);

							//update the fat
							fat.blocks[l] = 1;
							write_allTable(&fat);

						
							if(bytesLeft>(BLOCK_SIZE-sizeof(long)))
							{
								fwrite(buf+siz, BLOCK_SIZE-sizeof(long), 1, disk);
								file.fsize+=(BLOCK_SIZE-sizeof(long));
								siz+=(BLOCK_SIZE-sizeof(long));
								bytesLeft-=(BLOCK_SIZE-sizeof(long));
							}
							else
							{
								fwrite(buf+siz, bytesLeft, 1, disk);
								file.fsize+=(bytesLeft);
								siz+=(bytesLeft);
								bytesLeft=0;
								break;
							}
						}
						else
						{
							return -EINVAL;
						}					
					}
				}	
				
			}
			//all blocks have been read onto the buffer, close the file
			fclose(disk);

			//send all changes to disk
			dirEntry.files[j] = file;
			write_dirEntry(&dirEntry, dir.nStartBlock);
			write_root(&root);
		}
		else
		{
			//The file was not found, 
			//Else return that path doesn't exist
			res = -ENOENT;
		}
	}
	else
	{
		//The directory was not found, 
		//Else return that path doesn't exist
		res = -ENOENT;
	}


	//check to make sure path exists
	//check that size is > 0
	//check that offset is <= to the file size
	//read in data
	//set size and return, or error

	return siz;
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
