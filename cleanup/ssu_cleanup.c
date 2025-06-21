#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/wait.h>


// 학번과 최대 경로/파일 이름 길이를 정의.
#define STUDENT_ID "20211519"
#define MAX_PATH 4096
#define MAX_NAME 256

// FileNode 구조체: 파일의 정보(전체 경로, 파일명, 확장자, 수정 시간 등)를 저장
// 링크드 리스트로 파일 목록을 관리
typedef struct FileNode{
	char fullPath[PATH_MAX];
	char fileName[MAX_NAME];
	char extension[MAX_NAME];
	time_t mtime;
	int handled;               // 중복 처리 여부(0: 미처리, 1: 처리됨)
	struct FileNode *next;     // 다음 파일 노드
} FileNode;

// 트리 출력 시 전체 디렉토리와 파일의 개수를 저장하는 전역 변수
static int dirCount = 0;
static int fileCount = 0;

// tree 명령어 출력 시 옵션 설정 변수 (파일 크기, 권한 출력 여부)
static int showSize = 0;
static int showPerm = 0;

//함수 선언
void command_help(void);
void command_help_tree(void);
void command_help_arrange(void);
void command_help_exit(void);

void command_tree(char* dirPath, char* option);
void command_arrange(char* dirPath, int argc, char **argv);

int validateHomePath(const char* inputPath, char* resolvedPath);
void getPermissionString(mode_t mode, char* permStr);
void printTree(const char* path, const char* prefix, bool isLast);
static void getFileExtension(const char *filename, char *extBuf);
static bool isExcluded(const char *name, char **excludes, int excludeCount);
static bool isAllowedExtension(const char *ext, char **extFilters, int extCount);
void gatherFiles(const char *basePath, FileNode **head, time_t threshold, char **excludes, int excludeCount, char **extFilters, int extCount);
static int makeDirIfNotExist(const char *path);
int copyFile(const char *src, const char *dst);
void copyFiles(FileNode *head, const char *outputDir, int *copiedCount);
static void freeFileList(FileNode *head);
void handleDuplicateGroup(FileNode* dupList[], int dupCount, const char *outputDir, int *copiedCount);
int mkdirRecursive(const char *path, mode_t mode);


// 전체 도움말을 출력 (모든 명령어에 대한 사용법 안내)
void command_help(void) {
	printf("Usage:\n");
	printf(" > tree <DIR_PATH> [OPTION]...\n");
	printf("   <none> : Display the directory structure recursively if <DIR_PATH> is a directory\n");
	printf("   -s : Display the directory structure recursively if <DIR_PATH> is a directory, including the size of each file\n");
	printf("   -p: Display the directory structure recursively if <DIR_PATH> is a directory, including the permissions of each directory and file\n");
	printf(" > arrange <DIR_PATH> [OPTION]...\n");
	printf("   <none> : Arrange the directory if <DIR_PATH> is a directory\n");
	printf("   -d <output_path> : Specify the output directory <output_path> where <DIR_PATH> will be arranged if <DIR_PATH> is a directory\n");
	printf("   -t <seconds> : Only arrange files that were modified more than <seconds> seconds ago\n");
	printf("   -x <exclude_path1, exclude_path2, ...> : Arrange the directory if <DIR_PATH> is a directory except for the files inside <exclude_path> directory\n");
	printf("   -e <extension1, extension2, ...> : Arrange the directory with the specified extension <extension1, extension2, ...>\n");
	printf(" > help [COMMAND]\n");
	printf(" > exit\n");
}

// tree 명령어에 대한 상세 도움말 출력
void command_help_tree(void) {
	printf("Usage:\n");
	printf(" > tree <DIR_PATH> [OPTION]...\n");
	printf("   <none> : Display the directory structure recursively if <DIR_PATH> is a directory\n");
	printf("   -s : Display the directory structure recursively if <DIR_PATH> is a directory, including the size of each file\n");
	printf("   -p: Display the directory structure recursively if <DIR_PATH> is a directory, including the permissions of each directory and file\n");
}

// arrange 명령어에 대한 상세 도움말 출력
void command_help_arrange(void) {
	printf("Usage:\n");
	printf(" > arrange <DIR_PATH> [OPTION]...\n");
	printf("   <none> : Arrange the directory if <DIR_PATH> is a directory\n");
	printf("   -d <output_path> : Specify the output directory <output_path> where <DIR_PATH> will be arranged if <DIR_PATH> is a directory\n");
	printf("   -t <seconds> : Only arrange files that were modified more than <seconds> seconds ago\n");
	printf("   -x <exclude_path1, exclude_path2, ...> : Arrange the directory if <DIR_PATH> is a directory except for the files inside <exclude_path> directory\n");
	printf("   -e <extension1, extension2, ...> : Arrange the directory with the specified extension <extension1, extension2, ...>\n");
}

// exit 명령어 도움말 출력
void command_help_exit(void) {
	printf("Usage:\n");
	printf(" > exit\n");
	printf("   Exits the ssu_cleanup program. \n");
}


// 파일 모드(mode)를 받아 문자열(예: "drwxr-xr-x")로 변환
void getPermissionString(mode_t mode, char* permStr) {
	// 파일 종류 판별: 디렉토리이면 'd', 심볼릭 링크이면 'l', 그 외엔 일반 파일('-')로 표시
	if (S_ISDIR(mode))
		permStr[0] = 'd';
	else if (S_ISLNK(mode))
		permStr[0] = 'l';
	else
		permStr[0] = '-';

	// 사용자(owner) 권한 설정: 읽기, 쓰기, 실행 권한 여부에 따라 각각 'r', 'w', 'x' 또는 '-'로 표시
	permStr[1] = (mode & S_IRUSR) ? 'r' : '-';
	permStr[2] = (mode & S_IWUSR) ? 'w' : '-';
	permStr[3] = (mode & S_IXUSR) ? 'x' : '-';

	// 그룹 권한 설정: 읽기, 쓰기, 실행 권한 여부에 따라 각각 'r', 'w', 'x' 또는 '-'로 표시
	permStr[4] = (mode & S_IRGRP) ? 'r' : '-';
	permStr[5] = (mode & S_IWGRP) ? 'w' : '-';
	permStr[6] = (mode & S_IXGRP) ? 'x' : '-';

	// 다른 사용자(other) 권한 설정: 읽기, 쓰기, 실행 권한 여부에 따라 각각 'r', 'w', 'x' 또는 '-'로 표시
	permStr[7] = (mode & S_IROTH) ? 'r' : '-';
	permStr[8] = (mode & S_IWOTH) ? 'w' : '-';
	permStr[9] = (mode & S_IXOTH) ? 'x' : '-';

	// 문자열의 끝을 표시하는 NULL 문자 추가
	permStr[10] = '\0';
}

// 재귀적으로 디렉토리 트리 구조를 출력하는 함수
// path: 현재 디렉토리의 경로
// prefix: 출력할 때 앞에 붙일 접두사 (트리 모양을 유지하기 위해 사용)
// isLast: 현재 항목이 마지막 항목인지 여부 (출력 기호 결정에 사용)
void printTree(const char* path, const char* prefix, bool isLast) {
	// 주어진 경로의 디렉토리를 열기
	DIR* dir = opendir(path);
	if (!dir) {
		// 디렉토리 열기에 실패하면 에러 메시지를 출력하고 함수 종료
		perror(path);
		return;
	}

	// 현재 디렉토리의 파일 및 서브디렉토리 목록을 정렬하여 가져옴
	struct dirent** namelist = NULL;
	int count = 0, i = 0;
	count = scandir(path, &namelist, NULL, alphasort);
	if (count < 0) {
		// scandir 호출 실패 시 에러 출력 후 디렉토리 닫기
		perror("scandir");
		closedir(dir);
		return;
	}

	// 디렉토리 내 모든 항목에 대해 순회
	for (i = 0; i < count; i++) {
		struct dirent* entry = namelist[i];

		// 현재 디렉토리(.) 및 상위 디렉토리(..) 그리고 숨김 파일은 건너뜀
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			free(entry);
			continue;
		}
		
		// 숨김 파일 ('.'로 시작하는 파일)은 건너뜀
		if (entry->d_name[0] == '.') {
			free(entry);
			continue;
		}
	
		// 임시로 '~'로 끝나는 파일도 건너뜀
		size_t len = strlen(entry->d_name);
		if (len > 0 && entry->d_name[len - 1] == '~') {
			free(entry);
			continue;
		}

		// 현재 디렉토리의 전체 경로를 생성
		char newPath[PATH_MAX];
		snprintf(newPath, sizeof(newPath), "%s/%s", path, entry->d_name);

		// 해당 항목의 파일 정보를 가져옴
		struct stat st;
		if (lstat(newPath, &st) < 0) {
			perror("lstat");
			free(entry);
			continue;
		}

		// 트리 구조 출력: prefix와 함께 현재 항목의 구분 기호 출력
		printf("%s", prefix);
		if (i == count - 1) {

			printf("└─ ");
		}
		else {

			printf("├─ ");
		}
		
		// 옵션에 따라 파일 크기와 권한을 함께 출력
		// showPerm과 showSize는 전역 변수로 가정 (권한 및 크기 출력 여부 결정)
		if(showPerm && showSize){
			char permStr[11];
			getPermissionString(st.st_mode, permStr);
			// 권한 정보와 파일 크기를 함께 출력
			printf("[%s %lld] ", permStr, (long long)st.st_size);
		}
		else if (showPerm) {
			char permStr[11];
			getPermissionString(st.st_mode, permStr);
			// 권한 정보만 출력
			printf("[%s] ", permStr);
		}

		else if (showSize) {
			// 파일 크기만 출력
			printf("[%lld] ", (long long)st.st_size);
		}

		// 파일(또는 디렉토리) 이름 출력, 디렉토리일 경우 끝에 "/" 추가
		printf("%s", entry->d_name);
		if (S_ISDIR(st.st_mode)) {
			printf("/");
		}
		printf("\n");

		// 디렉토리일 경우 재귀 호출로 하위 항목 출력
		if (S_ISDIR(st.st_mode)) {
			dirCount++;

			char childPrefix[PATH_MAX];
			snprintf(childPrefix, sizeof(childPrefix), "%s%s",
				prefix, (i == count - 1) ? "   " : "│   ");
			printTree(newPath, childPrefix, (i == count - 1));
		}
		else {
			// 파일이면 파일 카운트 증가
			fileCount++;
		}
		// 동적으로 할당된 메모리 해제
		free(entry);
	}
	// scandir에서 할당한 배열 전체 해제
	free(namelist);
	// 열린 디렉토리 스트림 닫기
	closedir(dir);
}

// 입력받은 경로를 절대 경로로 변환한 후, 해당 경로가 사용자의 홈 디렉토리 하위에 있는지 확인
int validateHomePath(const char* inputPath, char* resolvedPath) {
	// inputPath를 절대 경로로 변환하여 resolvedPath에 저장
	if(realpath(inputPath, resolvedPath) == NULL){
		return 1;
	}
	// 환경 변수 "HOME"에서 사용자 홈 디렉토리 경로를 가져옴
	const char* home = getenv("HOME");
	if (home == NULL) {
		fprintf(stderr, "Failed to get HOME environment.\n");
		exit(EXIT_FAILURE);
	}
	// 홈 디렉토리 경로도 절대 경로로 변환하여 resolvedHome 배열에 저장
	char resolvedHome[PATH_MAX];
	if (realpath(home, resolvedHome) == NULL) {
		perror("realpath(home)");
		exit(EXIT_FAILURE);
	}
	// 홈 디렉토리 경로의 길이를 계산
	size_t homeLen = strlen(resolvedHome);

	// resolvedPath의 앞부분이 resolvedHome과 일치하는지 비교
	// 일치하지 않으면 입력 경로가 홈 디렉토리 하위에 있지 않음
	if (strncmp(resolvedHome,resolvedPath, homeLen) != 0) {
		return -1;
	}
	
	return -0;    
}


void command_tree(char* dirPath, char* option) {
	// 절대 경로를 저장할 버퍼 선언
	char resolvedPath[PATH_MAX];
	// 입력받은 경로(dirPath)를 절대 경로로 변환 후, 홈 디렉토리 내에 있는지 확인
	int ret = validateHomePath(dirPath, resolvedPath);
	if(ret == 1){
		// 경로 변환에 실패한 경우, 트리 명령어 도움말을 출력 후 종료
		command_help_tree();
		return;
	}else if(ret == -1){
		// 변환된 경로가 홈 디렉토리 하위에 있지 않은 경우 메시지 출력 후 종료
		printf("<%s> is outside the home directory\n", dirPath);
		return;
	}

	// 전역 변수 초기화: 디렉토리 및 파일 개수, 옵션 설정 초기화
	dirCount = 0;
	fileCount = 0;
	showSize = 0;
	showPerm = 0;


	// 옵션 문자열이 NULL이 아닌 경우, 옵션에 따라 출력 설정 조정
	if (option != NULL) {
		if (strcmp(option, "-s") == 0) {
			// "-s": 파일 크기 출력 옵션 활성화
			showSize = 1;
		}
		else if (strcmp(option, "-p") == 0) {
			// "-p": 파일 권한 출력 옵션 활성화
			showPerm = 1;
		}
		else if ((strcmp(option, "-sp") == 0) || (strcmp(option, "-ps") == 0)) {
			// "-sp" 또는 "-ps": 파일 크기와 권한 모두 출력 옵션 활성화
			showSize = 1;
			showPerm = 1;
		}
		else {
			// 유효하지 않은 옵션인 경우, 도움말을 출력 후 종료
			command_help_tree();
			return;
		}
	}

	// 입력된 절대 경로의 파일 정보를 가져오기 위해 lstat 호출
	struct stat st;
	if (lstat(resolvedPath, &st) < 0) {
		perror("lstat");
		return;
	}

	// 초기 경로가 디렉토리이면 디렉토리 카운트 증가, 아니면 파일 카운트 증가
	if (S_ISDIR(st.st_mode))
		dirCount++;
	else
		fileCount++;

	// 옵션에 따라 초기 경로의 권한 정보와 파일 크기를 출력
	if (showPerm && showSize) {
		char permStr[11];
		// 파일 모드를 문자열(예: "drwxr-xr-x")로 변환
		getPermissionString(st.st_mode, permStr);
		printf("[%s %lld] ", permStr, (long long)st.st_size);
	}
	else if (showPerm) {
		char permStr[11];
		// 파일 권한만 문자열로 변환 후 출력
		getPermissionString(st.st_mode, permStr);
		printf("[%s] ", permStr);
	}
	else if (showSize) {
		// 파일 크기만 출력
		printf("[%lld] ", (long long)st.st_size);
	}
	// 절대 경로를 출력, 디렉토리인 경우 '/' 추가
	printf("%s", resolvedPath);
	if (S_ISDIR(st.st_mode))
		printf("/");
	printf("\n");

	// 만약 초기 경로가 디렉토리이면, 재귀적으로 트리 구조를 출력
	if (S_ISDIR(st.st_mode))
		printTree(resolvedPath, "", true);
	// 최종적으로 총 디렉토리 수와 파일 수 출력
	printf("\n%d directories, %d files\n", dirCount, fileCount);
}

// 파일명에서 확장자를 추출하여 extBuf에 저장
// 확장자가 없으면 "none"을 저장
static void getFileExtension(const char *filename, char *extBuf){
	const char *dot = strrchr(filename, '.');
	if(!dot || dot == filename){
		strcpy(extBuf, "none");
	}else{
		strcpy(extBuf, dot + 1);
	}	
}

// 이름이 excludes 배열에 포함되어 있는지 검사
static bool isExcluded(const char *name, char **excludes, int excludeCount){
	if(excludes == NULL || excludeCount == 0 ){
		return false;
	}
	for(int i = 0; i < excludeCount; i++){
		if(strcmp(name, excludes[i]) == 0){
			return true;
		}
	}
	return false;
}

// 확장자가 extFilters 배열에 포함되어 있는지 검사
// extFilters가 없으면 항상 허용(true)함
static bool isAllowedExtension(const char *ext, char **extFilters, int extCount){
	if(extFilters == NULL || extCount == 0){
		return true;
	}
	for(int i = 0; i < extCount; i++){
		if(strcmp(ext, extFilters[i]) == 0 ){
			return true;
		}
	}
	return false;
}

//basePath부터 시작하여 재귀적으로 디렉토리 트리를 탐색하면서 조건에 맞는 파일들을 수집하여 FileNode 연결 리스트에 추가하는 함수
void gatherFiles(const char *basePath, FileNode **head, time_t threshold, char **excludes, int excludeCount, char **extFilters, int extCount){
	DIR *dir;
	struct dirent *entry;
	struct stat st;
	char pathBuf[PATH_MAX]; // 파일이나 디렉토리의 전체 경로를 저장할 버퍼

	// basePath 디렉토리를 열어 탐색 시작
	if((dir = opendir(basePath)) == NULL){
		// 디렉토리를 열 수 없으면 (예: 권한 부족 등) 함수 종료
		return;
	}

	// 디렉토리 내의 모든 항목들을 순차적으로 읽어옴
	while((entry = readdir(dir)) != NULL){
		// 현재 디렉토리(.)와 상위 디렉토리(..)는 무시
		if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		// basePath와 현재 항목의 이름을 결합하여 전체 경로를 생성
		snprintf(pathBuf, sizeof(pathBuf), "%s/%s", basePath, entry->d_name);

		// lstat을 사용해 pathBuf에 해당하는 항목의 파일 상태 정보를 가져옴
		if(lstat(pathBuf, &st) < 0 ){
			// 파일 정보를 가져오지 못하면, 해당 항목은 건너뜀
			continue;
		}
		// 항목이 디렉토리인 경우
		if(S_ISDIR(st.st_mode)){
			// 디렉토리 이름이 excludes 리스트에 포함되어 있지 않으면 재귀 호출
			if(!isExcluded(entry->d_name, excludes, excludeCount)){
				gatherFiles(pathBuf, head, threshold, excludes, excludeCount, extFilters, extCount);
			}
			// 항목이 일반 파일인 경우
		}else if(S_ISREG(st.st_mode)){
			// 현재 시간과 파일의 최종 수정 시간의 차이를 계산
			time_t now = time(NULL);
			double diff = difftime(now, st.st_mtime);
			// threshold 값이 양수이고, 차이가 threshold 이상이면 이 파일은 건너뜀
			if(threshold > 0 && diff >= threshold){
				continue;
			}
			// 파일의 확장자 추출을 위해 임시 버퍼 선언
			char extBuf[128];
			// 파일 이름에서 확장자를 추출하여 extBuf에 저장
			getFileExtension(entry->d_name, extBuf);
			// 확장자 필터가 설정되어 있고, 파일의 확장자가 허용되지 않은 경우 이 파일은 건너뜀
			if(!isAllowedExtension(extBuf, extFilters, extCount)){
				continue;
			}
			// 조건을 만족하는 파일에 대해 새로운 FileNode를 할당
			FileNode *newNode = (FileNode*)malloc(sizeof(FileNode));
			// 전체 경로를 FileNode에 복사
			strcpy(newNode->fullPath, pathBuf);
			// 파일 이름을 FileNode에 복사
			strcpy(newNode->fileName, entry->d_name);
			// 추출한 확장자를 FileNode에 복사
			strcpy(newNode->extension, extBuf);
			// 최종 수정 시간 저장
			newNode->mtime = st.st_mtime;
			// 아직 파일 처리가 되지 않았음을 나타내는 플래그 초기화
			newNode->handled = 0;
			// 새 노드를 현재 연결 리스트의 앞부분에 연결
			newNode->next = (*head);
			(*head) = newNode;
		
			
		}
	}
	// 디렉토리 스트림을 닫아 자원 해제
	closedir(dir);
}

//지정한 경로(path)가 존재하지 않으면 재귀적으로 디렉토리를 생성
static int makeDirIfNotExist(const char *path){
	struct stat st;
	// stat 함수를 사용해 경로에 대한 정보를 가져옴
	if(stat(path, &st) == 0){
		// 경로가 존재하는 경우, 디렉토리인지 확인
		if(!S_ISDIR(st.st_mode)){
			// 경로는 존재하지만 디렉토리가 아닌 경우 에러 메시지 출력 후 -1 반환
			fprintf(stderr, "%s exists but is not a directory.\n", path);
			return -1;
		}
		// 경로가 디렉토리인 경우, 아무런 작업도 하지 않고 0 반환
		return 0;
	}
	// 경로가 존재하지 않는 경우, 재귀적으로 디렉토리를 생성
	if(mkdirRecursive(path, 0775) < 0){
		// 디렉토리 생성 실패 시 perror로 에러 출력 후 -1 반환
		perror("mkdirRecursive");
		return -1;
	}
	return 0;
}

//src 경로에 위치한 파일을 dst 경로로 복사
// 파일을 바이너리 모드("rb", "wb")로 열어 데이터를 읽고 씀
int copyFile(const char *src, const char *dst){
	// 원본 파일을 읽기 전용 바이너리 모드로 염
		FILE *in = fopen(src, "rb");
		if(in == NULL){
			perror("fopen(src)");
			return -1;
		}
		// 대상 파일을 쓰기 전용 바이너리 모드로 염
		FILE *out = fopen(dst, "wb");
		if(out == NULL){
			perror("fopen(dst)");
			fclose(in); // 대상 파일 열기에 실패하면 원본 파일 스트림을 닫음
			return -1;
		}
		// 파일 데이터를 전송할 버퍼를 선언
		char buffer[BUFSIZ];
		size_t bytes;
		// 원본 파일에서 데이터를 읽어 버퍼에 저장하고, 읽은 바이트 수만큼 대상 파일에 씀
		while ((bytes = fread(buffer, 1, sizeof(buffer), in)) > 0){
			// fwrite가 읽은 바이트 수만큼 데이터를 쓰지 못하면 오류 처리
			if(fwrite(buffer, 1, bytes, out) != bytes){
				perror("fwrite");
				fclose(in);
				fclose(out);
				return -1;
			}
		}
		// 파일 복사가 완료되면, 열린 파일 스트림들을 닫음
		fclose(in);
		fclose(out);
		return 0;
}

// FileNode 연결 리스트에 저장된 파일들을 outputDir 경로 아래에 복사하는 함수
//파일들은 확장자에 따라 하위 디렉토리를 생성하여 저장
//동일한 이름과 확장자를 가진 파일들이 중복으로 존재할 경우, handleDuplicateGroup 함수를 호출하여 별도로 처리
void copyFiles(FileNode *head, const char *outputDir, int *copiedCount){
	// 연결 리스트를 순회하기 위한 포인터 초기화
		FileNode *cur = head;
		// 확장자별 하위 디렉토리 경로와 새 파일 경로를 저장할 버퍼 선언
		char extDir[PATH_MAX];
		char newPath[PATH_MAX];

		// FileNode 연결 리스트의 각 노드에 대해 처리
		while (cur) {
			// 이미 처리된 노드는 건너뛰기
			if (cur->handled) {
				cur = cur->next;
				continue;
			}

			// 현재 노드와 동일한 파일 이름과 확장자를 가진 중복 파일들을 찾기 위해,
		    // dupList 배열과 dupCount 카운터를 사용 (최대 100개의 중복 파일)
			FileNode* dupList[100];
			int dupCount = 0;
			dupList[dupCount++] = cur;

			// 현재 노드 이후의 노드들을 순회하며 중복 파일들을 찾음
			FileNode* temp = cur->next;
			while (temp) {
				if (!temp->handled && strcmp(temp->fileName, cur->fileName) == 0 && strcmp(temp->extension, cur->extension) == 0) {
					dupList[dupCount++] = temp;
				}
				temp = temp->next;
			}
			// 중복 파일 그룹에 단 하나의 파일만 존재하면 단일 파일 복사 처리
			if (dupCount == 1) {
				// outputDir 아래에 파일 확장자별 디렉토리를 생성하기 위한 경로를 작성
				int ret = snprintf(extDir, sizeof(extDir), "%s/%s", outputDir, cur->extension);
				if (ret < 0 || ret >= (int)sizeof(extDir)) {
					fprintf(stderr, "Error: extension directory path is too long.\n");
					cur->handled = 1;
					cur = cur->next;
					continue;
				}
				// 확장자별 디렉토리가 존재하지 않으면 생성 (makeDirIfNotExist 함수 사용)
				if(makeDirIfNotExist(extDir) < 0 ){
					cur->handled = 1;
					cur = cur->next;
					continue;
				}
				// 새 파일 경로를 생성
				ret = snprintf(newPath, sizeof(newPath), "%s/%s/%s", outputDir, cur->extension, cur->fileName);
				if (ret < 0 || ret >= (int)sizeof(newPath)) {
					fprintf(stderr, "Error: new file path is to long.\n");
					cur->handled = 1;
					cur = cur->next;
					continue;
				}
				// 원본 파일(cur->fullPath)을 새 경로(newPath)로 복사
				if (copyFile(cur->fullPath, newPath) != 0) {
					fprintf(stderr, "Failed to copy %s to %s\n", cur->fullPath, newPath);
				}
				if (copyFile(cur->fullPath, newPath) == 0) {
					(*copiedCount)++;
				}
				// 단일 파일 처리 완료 후 현재 노드를 처리됨으로 표시
				cur->handled = 1;
			}
			else {
				handleDuplicateGroup(dupList, dupCount, outputDir, copiedCount);

			}	
			// 다음 노드로 이동
			cur = cur->next;
			continue;
		}
}

//주어진 경로(path)에 대해, 존재하지 않는 모든 상위 디렉토리를 순차적으로 생성
int mkdirRecursive(const char *path, mode_t mode){
		char tmp[PATH_MAX]; // 입력받은 경로를 수정하기 위해 임시 버퍼에 복사
		char *p = NULL;  // 경로 문자열을 순회할 포인터
		size_t len;

		// path를 tmp 버퍼에 복사
		snprintf(tmp, sizeof(tmp), "%s",path);
		len = strlen(tmp);
		if(tmp[len - 1] == '/')
			tmp[len - 1] = '\0';
		// 경로 문자열의 두 번째 문자부터 순회
	// (첫 번째 문자는 '/'일 수 있으므로 tmp + 1부터 시작)
		for(p = tmp + 1; *p; p++){
			if(*p == '/'){
				*p = '\0';
				if(mkdir(tmp, mode) != 0){
					if(errno != EEXIST)
						return -1;
				}
				*p = '/';
			}
		}
		// 최종적으로 전체 경로에 대해 디렉토리 생성 시도
		if(mkdir(tmp, mode) != 0){
			if(errno != EEXIST)
				return -1;
		}
		return 0;
}
//FileNode 구조체로 구성된 연결 리스트의 모든 노드를 순회하며, 동적으로 할당된 메모리를 해제하는 함수
static void freeFileList(FileNode *head){
	FileNode *temp;
	// 리스트의 모든 노드를 순회하면서, 현재 노드를 임시 변수에 저장 후 해제
	while(head){
		temp = head;
		head = head->next;
		free(temp);
	}
}

//중복 파일 처리 함수
void handleDuplicateGroup(FileNode* dupList[], int dupCount, const char *outputDir, int *copiedCount) {
	
	// 무한 루프를 통해 사용자로부터 올바른 명령을 받을 때까지 반복
	while(1){
		// 중복 파일 목록 출력: 각 파일의 전체 경로를 번호와 함께 표시
		for (int i = 0; i < dupCount; i++) {
			printf("%d. %s\n", i + 1, dupList[i]->fullPath);
		}

		// 사용자에게 가능한 명령 옵션 안내 메시지 출력
		printf("\nchoose an option:\n");
		printf("0. select [num]\n");
		printf("1. diff [num] [num2]\n");
		printf("2. vi [num]\n");
		printf("3. do not select\n");

		// 프롬프트 출력
		printf("\n%s> ", STUDENT_ID);
		fflush(stdout);

		// 사용자 입력을 위한 버퍼 선언 및 입력 받기
		char line[256];
		if (!fgets(line, sizeof(line), stdin)) {
			// 입력이 없을 경우, 중복 파일들을 스킵하고 프로그램 종료
			printf("No input. Skipping these duplicates.\n");
			exit(EXIT_SUCCESS);
		}
		// 입력된 문자열에서 개행 문자 제거
		line[strcspn(line, "\n")] = '\0';  

		// 사용자가 "do not select" 명령어를 입력한 경우,
		// 그룹 내 모든 파일을 처리된 것으로 표시하고 함수 종료
		if (strcmp(line, "do not select") == 0) {
		
			for(int i = 0; i < dupCount; i++){
				dupList[i]->handled = 1;
			}
			return;
		}
		// 입력된 줄에서 첫 번째 토큰(명령어)을 추출
		char* cmd = strtok(line, " ");
		if (!cmd) {
			printf("No command entered. Skipping.\n");
			continue;
		}
		// "select" 명령어 처리: 특정 번호의 파일을 선택하여 복사
		if (strcmp(cmd, "select") == 0) {
	
			char* numStr = strtok(NULL, " ");
			if (!numStr) {
				printf("Usage: select [num]\n");
				continue;
			}
			int sel = atoi(numStr);
			// 입력된 번호가 유효한 범위(1 ~ dupCount)인지 확인
			if (sel < 1 || sel > dupCount) {
				printf("Invalid selection.\n");
				continue;
			}
			// 선택된 파일의 FileNode 포인터를 가져옴
			FileNode *selected = dupList[sel - 1];
			char extDir[PATH_MAX];
			char newPath[PATH_MAX];

			// outputDir 밑에 선택된 파일의 확장자 폴더 경로 생성 
			int ret = snprintf(extDir, sizeof(extDir), "%s/%s", outputDir, selected->extension);
			if(ret < 0 || ret >=(int)sizeof(extDir)){
				fprintf(stderr, "Error: extension directory path is too long.\n");
				continue;
			}
			// 확장자 디렉토리가 존재하지 않으면 생성
			if(makeDirIfNotExist(extDir) < 0){
				fprintf(stderr, "Failed to create directory.\n");
				continue;
			}
			// 새 파일 경로 생성
			ret = snprintf(newPath, sizeof(newPath), "%s/%s", extDir,selected->fileName);
			if(ret < 0 || ret >=(int)sizeof(newPath)){
				fprintf(stderr, "Error: new file path is too long.\n");
				continue;
			}
			// 파일 복사를 수행
			if(copyFile(selected->fullPath, newPath) == 0){
				(*copiedCount)++;
			} else{
				fprintf(stderr, "Failed to copy.\n");
			}
			// 복사 후, 그룹 내 모든 파일을 처리된 것으로 표시하고 함수 종료
			for(int i = 0; i < dupCount; i++){
				dupList[i]->handled = 1;
			}
			return;
		}	
		// "diff" 명령어 처리
		else if (strcmp(cmd, "diff") == 0) {
			// diff [num] [num2]
			char* numStr1 = strtok(NULL, " ");
			char* numStr2 = strtok(NULL, " ");
			if (!numStr1 || !numStr2) {
				printf("Usage: diff [num] [num2]\n");
				continue;
			}
			int n1 = atoi(numStr1);
			int n2 = atoi(numStr2);
			// 입력된 번호들이 유효한 범위 내에 있는지 확인
			if (n1 < 1 || n1 > dupCount || n2 < 1 || n2 > dupCount) {
				printf("Invalid selection.\n");
				continue;
			}
			// 새로운 프로세스를 생성하여 diff 명령어 실행
			pid_t pid = fork();
			if (pid == 0) {
				// 자식 프로세스: diff 명령어 실행
				char* args[] = { "diff", dupList[n1 - 1]->fullPath, dupList[n2 - 1]->fullPath, NULL };
				execvp("diff", args);
				perror("execvp diff");
				exit(EXIT_FAILURE);
			}
			else if (pid < 0) {
				perror("fork");
				continue;
			}
			else {
				// 부모 프로세스: 자식 프로세스가 종료될 때까지 기다림
				int status;
				waitpid(pid, &status, 0);
			}
			continue; // 루프를 다시 시작하여 명령 입력 대기
		}
		// "vi" 명령어 처리
		else if (strcmp(cmd, "vi") == 0) {
			// vi [num]
			char* numStr = strtok(NULL, " ");
			if (!numStr) {
				printf("Usage: vi [num]\n");
				continue;
			}
			int sel = atoi(numStr);
			// 선택 번호의 유효성 검사
			if (sel < 1 || sel > dupCount) {
				printf("Invalid selection.\n");
				continue;
			}
			// 새로운 프로세스를 생성하여 vi 편집기 실행
			pid_t pid = fork();
			if (pid == 0) {
				// 자식 프로세스: vi 실행
				char* args[] = { "vi", dupList[sel - 1]->fullPath, NULL };
				execvp("vi", args);
				perror("execvp vi");
				exit(EXIT_FAILURE);
			}
			else if (pid < 0) {
				perror("fork");
				continue;
			}
			else {
				// 부모 프로세스: 자식 프로세스가 종료될 때까지 기다림
				int status;
				waitpid(pid, &status, 0);
			}
			continue; // 명령 실행 후 루프 재시작
		}
		// 알 수 없는 명령어 입력 처리
		else {
			printf("Unknown command.\n");
			continue;
		}
	}
}
//주어진 디렉토리(dirPath)를 기반으로 파일들을 정리(arrange)하는 명령
void command_arrange(char *dirPath, int argc, char **argv){
	// 절대 경로로 변환된 경로를 저장할 버퍼 선언
		char resolvedPath[PATH_MAX];
	
		// dirPath의 파일 상태 정보를 확인하기 위한 구조체 선언
		struct stat st;
		// 주어진 경로가 존재하는지 확인 (존재하지 않으면 에러 메시지 출력 후 종료)
		if(stat(dirPath, &st) < 0 ){
			printf("%s does not exist\n", dirPath);
			return;
		}
		// 주어진 경로가 디렉토리인지 확인 
		if(!S_ISDIR(st.st_mode)){
			printf("%s is not a directory\n", dirPath);
			return;
		}
		// 입력받은 dirPath를 절대 경로로 변환하고, 홈 디렉토리 내에 있는지 검증
		int ret = validateHomePath(dirPath,resolvedPath);
		if(ret == -1){
			printf("<%s> is outside the home directory\n", dirPath);
			return;
		} else if(ret == 1){
			printf("%s is invalid.\n", dirPath);
			return;
		}
		// 옵션 값들을 저장할 변수들 초기화
		char *outputPath = NULL;
		time_t olderThan = 0;
		char *excludeRaw = NULL;
		char *extRaw = NULL;

		// 옵션 인자 파싱: argv 배열을 순회하면서 각 옵션을 처리
		int i = 0;
		while(i < argc){
			if(strcmp(argv[i], "-d") == 0){
				// 출력 디렉토리 지정 옵션: 다음 인자를 outputPath로 저장
				i++;
				if(i >= argc){
					command_help_arrange();
					return;
				}
				outputPath = argv[i];
			}
			else if(strcmp(argv[i], "-t") == 0){
				// 시간 제한 옵션
				i++;
				if(i >= argc){
					command_help_arrange();
					return;
				}
				olderThan = atol(argv[i]);
			}
			else if(strcmp(argv[i], "-x") == 0 ){
				// 제외할 이름 옵션
				i++;
				if(i >= argc){
					command_help_arrange();
					return;
				}
				excludeRaw = argv[i];
			}
			else if(strcmp(argv[i], "-e") == 0 ){
				// 확장자 필터 옵션
				i++;
				if(i >= argc){
					command_help_arrange();
					return;
				}
				extRaw = argv[i];
			}
			else{
				// 인식할 수 없는 옵션이 있으면 도움말을 출력하고 종료
				command_help_arrange();
				return;
			}
			i++;
		}
		
		// 제외할 이름들을 저장할 배열과 개수 초기화 (최대 50개)
		char *excludes[50];
		int excludeCount = 0;
		if(excludeRaw){
			// 콤마(,)를 구분자로 문자열을 분리하여 배열에 저장
			char *tok = strtok(excludeRaw, ",");
			while(tok && excludeCount < 50){
				excludes[excludeCount++] = tok;
				tok = strtok(NULL, ",");
			}
		}
		// 허용할 확장자들을 저장할 배열과 개수 초기화 (최대 50개)
		char *extFilters[50];
		int extCount = 0;
		if(extRaw){
			// 콤마(,)를 구분자로 문자열을 분리하여 배열에 저장
			char *tok = strtok(extRaw, ",");
			while(tok && extCount < 50){
				extFilters[extCount++] = tok;
				tok = strtok(NULL, ",");
			}
		}
		// 최종 출력 디렉토리 경로(finalOutput)를 결정
		char finalOutput[PATH_MAX];
		if(outputPath == NULL){
			
			// 출력 디렉토리가 지정되지 않은 경우, 기본값으로 "[dirPath]_arranged" 사용
			int ret = snprintf(finalOutput, sizeof(finalOutput), "%s_arranged", dirPath);
			if(ret < 0 || ret >= (int)sizeof(finalOutput)){
				fprintf(stderr, "Error: output path is too long.\n");
				return;
			}
		}else{
			// 지정된 출력 디렉토리를 finalOutput에 복사
			strncpy(finalOutput, outputPath, sizeof(finalOutput));
		}

		// 파일 수집: gatherFiles 함수를 통해 조건에 맞는 파일들을 FileNode 연결 리스트에 저장
		FileNode *fileList = NULL;
		gatherFiles(dirPath, &fileList, olderThan, excludes, excludeCount, extFilters, extCount);

		// 파일 복사: 수집된 파일들을 finalOutput 디렉토리로 복사하고, 복사된 파일의 개수를 카운트
		int copiedCount = 0;
		copyFiles(fileList, finalOutput, &copiedCount);
		// 동적으로 할당된 FileNode 리스트 메모리 해제
		freeFileList(fileList);

		// 복사된 파일 수에 따라 결과 메시지 출력
		if(copiedCount >0){	
			printf("%s arranged\n", dirPath);
		} else{
			printf("No files arranged,\n");
		}
}


	


int main(void) {
	// 사용자 입력을 저장할 버퍼 선언 (최대 1024 문자)
	char input[1024];

	// 무한 루프: 사용자가 exit 명령을 입력할 때까지 반복
	while (1) {
		// 프롬프트 출력: STUDENT_ID는 사용자 식별 문자열
		printf("%s> ", STUDENT_ID);
		
		// 사용자 입력을 읽어옴. 입력이 없으면 루프 종료
		if (fgets(input, sizeof(input), stdin) == NULL)
			break;
		// 입력 문자열의 끝에 있는 개행 문자 제거
		input[strcspn(input, "\n")] = '\0';
	
		// 빈 문자열인 경우 다시 프롬프트 출력
		if (strlen(input) == 0)
			continue;

		// 입력 문자열을 공백(" ")을 기준으로 토큰화하여 첫 번째 토큰(명령어)을 추출
		char* command = strtok(input, " ");
		if (command == NULL)
			continue;
	
		// "help" 명령어 처리
		if (strcmp(command, "help") == 0) {
		
			char* subcommand = strtok(NULL, " ");
			if (subcommand == NULL) {
		
				command_help();
			}
			else if (strcmp(subcommand, "tree") == 0) {
				command_help_tree();
			}
			else if (strcmp(subcommand, "arrange") == 0) {
				command_help_arrange();
			}
			else if (strcmp(subcommand, "exit") == 0) {
				command_help_exit();
			}
			else {
				command_help();
			}
		}
		// "exit" 명령어 처리: 프로그램 종료
		else if (strcmp(command, "exit") == 0) {
		
			break;
		}
		// "arrange" 명령어 처리: 파일 정리 기능 실행
		else if (strcmp(command, "arrange") == 0) {
			char*dirPath = strtok(NULL, " ");
			if(dirPath == NULL){
				command_help_arrange();
				continue;
			}
			// arrange 명령어 뒤에 추가 옵션들을 저장할 배열 선언 (최대 50개 옵션)
			char *argvArr[50];
			int argc = 0;
			char *tok;
			// 남은 입력 토큰들을 argvArr 배열에 저장하고, 옵션 개수를 argc에 기록
			while((tok = strtok(NULL, " ")) != NULL){
				argvArr[argc++] = tok;
			}
			argvArr[argc] = NULL;
			// arrange 명령어를 처리하는 함수 호출
			command_arrange(dirPath, argc, argvArr);
		}

		// "tree" 명령어 처리: 디렉토리 트리 출력 기능 실행
		else if (strcmp(command, "tree") == 0) {

			// tree 명령어 뒤에 첫 번째 인자는 대상 디렉토리 경로
			char* dirPath = strtok(NULL, " ");
			if (dirPath == NULL) {
			
				command_help_tree();
				continue;
			}
			// tree 명령어 뒤에 두 번째 토큰은 추가 옵션
			char* option = strtok(NULL, " ");
			command_tree(dirPath, option);

		}
		// 인식되지 않은 명령어 처리: 기본 도움말 출력
		else {
			command_help();
		}
	}
	return 0;
}
