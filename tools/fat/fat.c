#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define ERR_FILE_NOT_FOUND -5
#define ERR_FILE_READ_FAIL -6

typedef struct{
    uint8_t BootJumpInstruction[3];
    uint8_t OemIdentifier[8];
    uint16_t BytesPerSector;
    uint8_t SectorsPerCluster;
    uint16_t ReservedSectors;
    uint8_t FatCount;
    uint16_t DirEntryCount;
    uint16_t TotalSectors;
    uint8_t MediaDescriptortype;
    uint16_t SectorsPerFat;
    uint16_t SectorsPerTrack;
    uint16_t Heads;
    uint32_t HiddenSectors;
    uint32_t LargeSectorCount;

    //extended boot record
    uint8_t DriverNumber;
    uint8_t Signature;
    uint16_t VolumeId; //serial number
    uint8_t VolumeLabel[11]; // 11bytes padded with spaces
    uint8_t SystemId[8]; // 8 bytes


}__attribute__((packed)) BootSector;

typedef struct{
    uint8_t Name[11];
    uint8_t Attributes;
    uint8_t _Reserved;
    uint8_t CreatedTimeTenth;
    uint16_t CreatedTime;
    uint16_t CreatedDate;
    uint16_t AccessedDate;
    uint16_t FirstClusterHigh;
    uint16_t ModifiedTime;
    uint16_t ModifiedDate;
    uint16_t FirstClusterLow;
    uint32_t Size;
}__attribute__((packed)) DirectoryEntry;

BootSector g_BootSector;
uint8_t* g_fat = NULL;
DirectoryEntry* g_RootDirectory = NULL;
uint32_t g_RootDirectoryEnd;


bool readBootSector(FILE* disk){
    return fread(&g_BootSector, sizeof(g_BootSector),1,disk)==1;
}

bool readSectors(FILE* disk, uint32_t lba, uint32_t count, void* bufferOut){
    bool ok = true;
    ok = ok && (fseek(disk,lba*g_BootSector.BytesPerSector,SEEK_SET)==0);
    ok = ok && (fread(bufferOut,g_BootSector.BytesPerSector,count,disk)==count);
    return ok;
}

bool readFat(FILE* disk){
    g_fat = (uint8_t*)malloc(g_BootSector.SectorsPerFat*g_BootSector.BytesPerSector);
    return readSectors(disk,g_BootSector.ReservedSectors,g_BootSector.SectorsPerFat,g_fat);
}

bool readRootDirectory(FILE* disk){
    uint32_t lba = g_BootSector.ReservedSectors + g_BootSector.SectorsPerFat * g_BootSector.FatCount;
    uint32_t size = sizeof(DirectoryEntry) * g_BootSector.DirEntryCount;
    uint32_t sectors = (size/g_BootSector.BytesPerSector);
    if(size%g_BootSector.BytesPerSector>0){
        sectors++;
    }

    g_RootDirectoryEnd = lba + sectors;
    g_RootDirectory = (DirectoryEntry*)malloc(sectors * g_BootSector.BytesPerSector);
    return readSectors(disk,lba,sectors,g_RootDirectory);
}

void formatFilename(const char* input, char* output) {
    // Initialize with spaces
    memset(output, ' ', 11);

    // Split name and extension
    const char* dot = strchr(input, '.');
    size_t nameLen = dot ? (dot - input) : strlen(input);
    size_t extLen  = dot ? strlen(dot + 1) : 0;

    // Copy name (up to 8 chars)
    for (size_t i = 0; i < nameLen && i < 8; i++) {
        output[i] = toupper((unsigned char)input[i]);
    }

    // Copy extension (up to 3 chars)
    for (size_t i = 0; i < extLen && i < 3; i++) {
        output[8 + i] = toupper((unsigned char)dot[1 + i]);
    }
}


DirectoryEntry* findFile(const char* name){
    char formatted[11];
    formatFilename(name, formatted);

    for(uint32_t i = 0; i<g_BootSector.DirEntryCount; i++){
        // Skip unused or deleted entries
        if (g_RootDirectory[i].Name[0] == 0x00) break;   // end of directory
        if (g_RootDirectory[i].Name[0] == 0xE5) continue; // deleted entry

        if(memcmp(formatted, g_RootDirectory[i].Name,11)==0){
            return &g_RootDirectory[i];
        }
    }
    return NULL;
}

bool readFile(DirectoryEntry* fileEntry, FILE* disk, uint8_t* outputBuffer){
    bool ok = true;
    uint16_t currentCluster = fileEntry -> FirstClusterLow;

    do{
        uint32_t lba = g_RootDirectoryEnd + (currentCluster - 2)*g_BootSector.SectorsPerCluster;
        ok = ok && readSectors(disk,lba,g_BootSector.SectorsPerCluster,outputBuffer);
        outputBuffer += g_BootSector.SectorsPerCluster * g_BootSector.BytesPerSector;

        uint32_t fatIndex = currentCluster * 3/2;
        if(currentCluster%2==0){
            currentCluster = (*(uint16_t*)(g_fat + fatIndex)) & 0x0FFF;
        }
        else{
            currentCluster = (*(uint16_t*)(g_fat+fatIndex))>>4;
        }
    }
    while(ok && currentCluster<0x0FF8);

    return ok;
}

int main(int argc, char** argv){

    if(argc<3){
        fprintf(stderr,"Syntax: %s <disk image> <file name>\n", argv[0]);
        return -1;
    }

    FILE* disk = fopen(argv[1],"rb");

    if(!disk){
        fprintf(stderr,"Cannot open disk image %s! \n",argv[1]);
        return -1;
    }

    if(!readBootSector(disk)){
        fprintf(stderr,"Could not read boot sector!\n");
        fclose(disk);
        return -2;
    }

    if(!readFat(disk)){
        fprintf(stderr,"Could not read FAT!\n");
        fclose(disk);
        return -3;
    }

    if(!readRootDirectory(disk)){
        fprintf(stderr,"Could not read Root Directory!\n");
        free(g_fat);
        free(g_RootDirectory);
        fclose(disk);
        return -4;
    }

    DirectoryEntry* fileEntry = findFile(argv[2]);

    if(!fileEntry){
        fprintf(stderr,"Could not find file %s!\n",argv[2]);
        free(g_fat);
        free(g_RootDirectory);
        fclose(disk);
        return -5;
    }

    uint8_t* buffer = (uint8_t*)malloc(fileEntry->Size+g_BootSector.BytesPerSector);
    if(!readFile(fileEntry, disk, buffer)){
        fprintf(stderr,"Could not read file %s!\n",argv[2]);
        free(g_fat);
        free(g_RootDirectory);
        fclose(disk);
        return -6;
    }

    for(size_t i=0; i<fileEntry->Size;i++){
        if(isprint(buffer[i])){
            fputc(buffer[i],stdout);
        }
        else{
            printf("<%02x>",buffer[i]);
        }
    }
    printf("\n");

    free(buffer);
    free(g_fat);
    free(g_RootDirectory);
    fclose(disk);

    return 0;
}