#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <fcntl.h>
#include <utime.h>
#include <ctype.h>
#include <signal.h>
#include <limits.h>

/*
 * SAFE_SNPRINTF 매크로
 *
 * snprintf 함수로 문자열 포매팅 시, 버퍼 오버플로우를 방지하기 위해 반환값을 검사.
 * - 만약 필요한 버퍼 크기가 현재 버퍼 크기보다 크거나 오류가 발생하면 에러 메시지를 출력하고 프로그램 종료.
 */
#define SAFE_SNPRINTF(buf, bufsize, fmt, ...)                          \
    do {                                                               \
        int _ret = snprintf((buf), (bufsize), (fmt), __VA_ARGS__);      \
        if (_ret < 0 || _ret >= (int)(bufsize)) {                        \
            fprintf(stderr, "Error: formatted string exceeds buffer size at %s:%d. Needed %d bytes, buffer size %zu.\n", \
                    __FILE__, __LINE__, _ret, (size_t)(bufsize));        \
            exit(EXIT_FAILURE);                                          \
        }                                                              \
    } while(0)


 // 상수 정의
#define MAX_PATH 4096      // 리눅스상 파일 경로의 최대 길이
#define MAX_FILENAME 255   // 파일 이름의 최대 길이
#define MY_MAX_INPUT 4096     // 사용자로부터 입력받는 버퍼의 최대 크기
#define MAX_LINE 1024      // 파일 I/O 시 한 줄의 최대 버퍼 크기

// 글로벌 변수: 로그 최대 줄 수 (-1이면 제한 없음)
int global_max_log_lines = -1;

/*
 * 데몬 프로세스 정보를 저장할 링크드 리스트 노드 구조체
 * 각 노드는 모니터링 대상 디렉토리의 절대경로와 해당 데몬의 프로세스 ID(PID)를 저장.
 */
typedef struct DaemonNode {
    char dirPath[MAX_PATH];      // 모니터링 대상 디렉토리의 절대경로
    pid_t pid;                   // 해당 데몬의 프로세스 ID
    struct DaemonNode* next;     // 다음 데몬 노드를 가리키는 포인터
} DaemonNode;
DaemonNode* daemonList = NULL; // 전체 데몬 프로세스 관리를 위한 링크드 리스트 (초기에는 비어 있음)


/*
 * 후보 파일 정보를 저장하는 링크드 리스트 노드 구조체
 * 파일 정리 시 정리할 파일 목록을 관리하며,
 * 파일 이름, 절대 경로, 수정 시간, 그리고 중복 여부를 저장.
 */
typedef struct CandidateNode {
    char filename[MAX_FILENAME];   // 파일 이름
    char fullPath[MAX_PATH];         // 파일의 절대 경로
    time_t mtime;                  // 파일의 최종 수정 시간
    int duplicate;                 // 0: 유일, 1: 중복 존재 (예: 모드 3에서 사용)
    struct CandidateNode* next;    // 다음 후보 노드 포인터
} CandidateNode;

CandidateNode* candidateList = NULL; // 후보 파일 리스트 (매번 스캔 시 새롭게 구성됨)


/*
 * 데몬 프로세스 설정 정보를 저장할 구조체
 * 설정 파일(ssu_cleanupd.config)에 기록되며, 모니터링 경로, 출력 경로, 주기, 로그 줄 수,
 * 제외 경로, 파일 확장자, 정리 모드, 시작 시간 등의 정보를 포함.
 */
typedef struct Config {
    char monitoring_path[MAX_PATH];  // 모니터링 대상 디렉토리
    char output_path[MAX_PATH];      // 정리(복사) 파일 출력 대상 디렉토리
    int time_interval;               // 모니터링 주기 (초 단위)
    char max_log_lines[64];          // 로그 파일 최대 줄 수 (문자열, "none"이면 제한 없음)
    char exclude_path[1024];         // 제외할 디렉토리 경로 (콤마로 구분된 목록)
    char extension[256];             // 정리 대상으로 할 파일 확장자 (콤마 구분)
    int mode;                      // 정리 모드 (1: 최신, 2: 오래된, 3: 중복시 정리하지 않음)
    char start_time[64];           // 데몬이 처음 추가된 시각 (수정되지 않음)
} Config;



/* 함수 프로토타입 선언 */
// 데몬 노드 관련
void insertDaemonNode(const char* dirPath, pid_t pid);
void restoreDaemonListFromFile(void);
void updateDaemonListFile(const char* path);
void updateDaemonListFileAfterRemoval(const char* removedPath);

// 파일/디렉토리 관련
int isDirectory(const char* path);
int isInsideHome(const char* absPath);
int isSubdirectory(const char* parent, const char* child);
void getExtension(const char* filename, char* extBuf, size_t bufSize);
int copyFile(const char* src, const char* dst);

// 로그 및 후보 관련
void writeLog(const char* logPath, pid_t pid, const char* src, const char* dst, const char* monitorPath);
void updateCandidate(const char* filename, const char* fullPath, time_t mtime, int mode);
void scanDirectoryForCandidates(const char* monitorPath, const char* excludePaths, const char* extensions, int mode);
void arrangeCandidates(const char* arrangedPath, const char* logPath, const char* monitorPath, pid_t daemonPid);
void freeCandidateList(void);

// config 및 log 파일 생성/수정 관련
void trimLogFile(const char* logPath, int maxLines);
void createConfigFile(const char* monitorPath, const char* arrangedPath, pid_t pid, int timeInterval, const char* maxLogLines, const char* excludePaths, const char* extensions, int mode);
void createLogFile(const char* monitorPath);
int getCurrentMaxLogLines(const char* monitorPath);
void updateConfigFile(const char* monitorPath, const char* arrangedPath, pid_t pid, int timeInterval, const char* maxLogLines, const char* excludePaths, const char* extensions, int mode, const char* existingStartTime);
int readConfigFileWithLock(const char* monitorPath, Config* config);

// 데몬 루프 및 명령어 처리 관련
void daemonProcessLoop(const char* absPath);
void initHomeDir(void);
void show_command(void);
void add_command(char* arguments);
void modify_command(char* arguments);
void remove_command(char* arguments);
void help_command(void);


/*
 * 데몬 노드를 링크드 리스트에 추가하는 함수
 * 인자로 받은 모니터링 경로와 pid를 기반으로 새로운 노드를 생성 후 리스트 맨 앞에 삽입.
 */
void insertDaemonNode(const char* dirPath, pid_t pid) {
    DaemonNode* newNode = (DaemonNode*)malloc(sizeof(DaemonNode));
    if (!newNode) {
        perror("malloc error");
        return;
    }
    // 주어진 경로와 pid를 복사
    strncpy(newNode->dirPath, dirPath, MAX_PATH);
    newNode->pid = pid;
    newNode->next = daemonList;
    daemonList = newNode;
}

/*
 * ~/.ssu_cleanupd/current_daemon_list 파일을 읽어 데몬 리스트(daemonList)를 복원.
 * 각 라인은 모니터링 대상 디렉토리의 절대경로이며,
 * 각 디렉토리 내의 ssu_cleanupd.config 파일에서 pid 정보를 파싱하여 리스트에 추가.
 */
void restoreDaemonListFromFile() {
    char daemonListFile[MAX_PATH];
    char buffer[MAX_LINE];
    char absPath[MAX_PATH];
    char* home = getenv("HOME");
    if (!home)
        return;
    SAFE_SNPRINTF(daemonListFile, sizeof(daemonListFile), "%s/.ssu_cleanupd/current_daemon_list", home);

    FILE* fp = fopen(daemonListFile, "r");
    if (fp == NULL)
        return;
    // 파일의 각 줄은 모니터링 대상 디렉토리의 절대경로
    while (fgets(buffer, sizeof(buffer), fp) != NULL) {
        // 개행 문자 제거
        buffer[strcspn(buffer, "\n")] = '\0';
        if (strlen(buffer) == 0)
            continue;
        // buffer에 있는 경로를 absPath에 복사
        strncpy(absPath, buffer, MAX_PATH);
        absPath[MAX_PATH - 1] = '\0';

        // 해당 디렉토리에 있는 ssu_cleanupd.config 파일 경로 구성
        char configPath[MAX_PATH];
        SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", absPath);
        FILE* fconfig = fopen(configPath, "r");
        if (fconfig) {
            int pid = 0;
            // config 파일에서 "pid :" 라인을 찾아 pid 값을 파싱한다.
            while (fgets(buffer, sizeof(buffer), fconfig)) {
                if (strncmp(buffer, "pid :", 5) == 0) {
                    // "pid : <pid_value>" 형식에서 숫자 부분 파싱
                    char* token = buffer + 5;
                    while (*token == ' ' || *token == '\t')
                        token++;
                    pid = atoi(token);
                    break;
                }
            }
            fclose(fconfig);
            if (pid > 0) {
                // 복원: 데몬 리스트에 노드를 추가
                insertDaemonNode(absPath, pid);
            }
        }
    }
    fclose(fp);
}

/*
 * 새로운 데몬 프로세스가 추가될 때, 해당 모니터링 경로를
 * ~/.ssu_cleanupd/current_daemon_list 파일에 추가로 기록.
 */
void updateDaemonListFile(const char* path) {
    char daemonListFile[MAX_PATH];
    char* home = getenv("HOME");
    if (!home) return;
    SAFE_SNPRINTF(daemonListFile, sizeof(daemonListFile), "%s/.ssu_cleanupd/current_daemon_list", home);
    FILE* fp = fopen(daemonListFile, "a");  // append 모드로 열기
    if (fp) {
        fprintf(fp, "%s\n", path);
        fclose(fp);
    }
}

/*
 * 데몬 프로세스 제거 시 호출되는 함수.
 * current_daemon_list 파일에서 제거된 디렉토리 경로에 해당하는 라인을 삭제하여 파일을 갱신.
 */
void updateDaemonListFileAfterRemoval(const char* removedPath) {
    char daemonListFile[MAX_PATH];
    char tempFile[MAX_PATH];
    char line[MAX_PATH];
    char* home = getenv("HOME");
    if (!home)
        return;
    SAFE_SNPRINTF(daemonListFile, sizeof(daemonListFile), "%s/.ssu_cleanupd/current_daemon_list", home);
    SAFE_SNPRINTF(tempFile, sizeof(tempFile), "%s/.ssu_cleanupd/temp_daemon_list", home);

    FILE* fp = fopen(daemonListFile, "r");
    if (!fp)
        return;
    FILE* tf = fopen(tempFile, "w");
    if (!tf) {
        fclose(fp);
        return;
    }
    // 기존 파일의 각 라인을 읽어 제거할 경로와 일치하지 않는 라인만 임시 파일에 기록
    while (fgets(line, sizeof(line), fp) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        if (strcmp(line, removedPath) != 0) {
            fprintf(tf, "%s\n", line);
        }
    }
    fclose(fp);
    fclose(tf);
    // 기존 파일을 삭제하고, 임시 파일을 원래 파일명으로 변경하여 갱신
    remove(daemonListFile);
    rename(tempFile, daemonListFile);
}


/*
 * 주어진 경로가 디렉토리인지 확인하는 함수
 * stat() 함수 호출을 통해 파일 정보를 얻은 후, S_ISDIR 매크로로 디렉토리 여부를 판단.
 */
int isDirectory(const char* path) {
    struct stat st;
    if (stat(path, &st) == -1)
        return 0;
    return S_ISDIR(st.st_mode);
}

/*
 * 주어진 절대 경로가 사용자의 홈 디렉토리 내에 있는지 확인
 */
int isInsideHome(const char* absPath) {
    char* home = getenv("HOME");
    if (!home) return 0;
    size_t homeLen = strlen(home);
    return (strncmp(absPath, home, homeLen) == 0);
}

/*
 * parent가 child의 상위 디렉토리(또는 동일한 디렉토리)인지 확인
 * parent 문자열이 child의 시작부분과 일치하면 상위(또는 동일)으로 판단
 */
int isSubdirectory(const char* parent, const char* child) {
    size_t plen = strlen(parent);
    if (strncmp(parent, child, plen) == 0) {
        if (child[plen] == '/' || child[plen] == '\0')
            return 1;
    }
    return 0;
}

/*
 * 파일 이름에서 확장자를 추출하는 함수
 * 만약 확장자가 없으면 "noext" 문자열을 복사.
 */
void getExtension(const char* filename, char* extBuf, size_t bufSize) {
    const char* dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        strncpy(extBuf, "noext", bufSize);
    }
    else {
        strncpy(extBuf, dot + 1, bufSize);
    }
    extBuf[bufSize - 1] = '\0';
}

void trimLogFile(const char* logPath, int maxLines) {
    FILE* fp = fopen(logPath, "r");
    if (!fp) return;

    char* lines[1000]; // 충분히 큰 배열
    int count = 0;
    char buffer[MAX_LINE];

    // 파일 전체를 읽어들임
    while (fgets(buffer, sizeof(buffer), fp) != NULL && count < 1000) {
        lines[count] = strdup(buffer);
        count++;
    }
    fclose(fp);

    // maxLines를 초과하는 경우, 나머지(오래된) 로그를 삭제
    int start = (count > maxLines) ? count - maxLines : 0;

    fp = fopen(logPath, "w");
    if (!fp) {
        for (int i = 0; i < count; i++) {
            free(lines[i]);
        }
        return;
    }
    for (int i = start; i < count; i++) {
        fputs(lines[i], fp);
        free(lines[i]);
    }
    // 사용하지 않은 메모리 해제
    for (int i = 0; i < start; i++) {
        free(lines[i]);
    }
    fclose(fp);
}

/*
 * src 파일을 dst 파일로 복사하는 함수.
 * 복사 후 원본 파일의 접근 및 수정 시간도 복원.
 */
int copyFile(const char* src, const char* dst) {
    int fdSrc = open(src, O_RDONLY);
    if (fdSrc < 0) return -1;
    int fdDst = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdDst < 0) {
        close(fdSrc);
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fdSrc, buf, sizeof(buf))) > 0) {
        if (write(fdDst, buf, n) != n) {
            close(fdSrc);
            close(fdDst);
            return -1;
        }
    }
    fsync(fdDst);
    close(fdSrc);
    close(fdDst);

    // 파일의 시간 정보를 복원: 원본 파일의 접근시간과 수정시간을 읽어 dst에 적용
    struct stat srcStat;
    if (stat(src, &srcStat) == 0) {
        struct utimbuf newTimes;
        newTimes.actime = srcStat.st_atime;
        newTimes.modtime = srcStat.st_mtime;
        utime(dst, &newTimes);
    }
    return 0;
}

int getCurrentMaxLogLines(const char* monitorPath) {
    Config config;
    // monitorPath가 데몬 모니터링 디렉토리 (absPath)라고 가정
    if (readConfigFileWithLock(monitorPath, &config) == 0) {
        // 만약 "none"이 아니라면 숫자로 변환
        if (strcmp(config.max_log_lines, "none") != 0) {
            return atoi(config.max_log_lines);
        }
    }
    return -1; // 유효하지 않다면 음수 반환
}

/*
 * 로그 기록 함수
 * 로그 파일에 [시간][pid][원본파일 경로][복사된 파일 경로] 형식의 로그를 추가.
 */
void writeLog(const char* logPath, pid_t pid, const char* src,const char* dst, const char* monitorPath) {
    time_t t = time(NULL);
    struct tm* tmInfo = localtime(&t);
    char timeBuf[64];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", tmInfo);

    FILE* fp = fopen(logPath, "a");
    if (!fp) return;
    fprintf(fp, "[%s][%d][%s][%s]\n", timeBuf, pid, src, dst);
    fclose(fp);

    // 최신 max log 줄 수를 config 파일로부터 읽어오기
    int currentMaxLines = getCurrentMaxLogLines(monitorPath);
    if (currentMaxLines > 0) {
        trimLogFile(logPath, currentMaxLines);
    }
}

/*
 * 파일 후보 리스트를 업데이트하는 함수.
 * 파일 스캔 시, 이미 동일 파일명이 후보 리스트에 존재하면:
 * - mode 1 (최신): 더 최근의 파일로 대체
 * - mode 2 (오래된): 더 오래된 파일로 대체
 * - mode 3: 중복 표시만 함 (duplicate 플래그를 1로 설정)
 * 후보가 없으면 새로운 노드를 추가.
 */
void updateCandidate(const char* filename, const char* fullPath, time_t mtime, int mode) {
    CandidateNode* curr = candidateList;
    while (curr != NULL) {
        if (strcmp(curr->filename, filename) == 0) {
            if (mode == 3) {
                curr->duplicate = 1;
            }
            else if (mode == 1) {  // 최신 파일 선택
                if (mtime > curr->mtime) {
                    strncpy(curr->fullPath, fullPath, MAX_PATH);
                    curr->mtime = mtime;
                }
            }
            else if (mode == 2) {  // 오래된 파일 선택
                if (mtime < curr->mtime) {
                    strncpy(curr->fullPath, fullPath, MAX_PATH);
                    curr->mtime = mtime;
                }
            }
            return;
        }
        curr = curr->next;
    }
    // 후보가 없으면 새로 추가
    CandidateNode* newNode = (CandidateNode*)malloc(sizeof(CandidateNode));
    if (!newNode) {
        perror("malloc error");
        return;
    }
    strncpy(newNode->filename, filename, MAX_FILENAME);
    strncpy(newNode->fullPath, fullPath, MAX_PATH);
    newNode->mtime = mtime;
    newNode->duplicate = 0;
    newNode->next = candidateList;
    candidateList = newNode;
}

/*
 * 재귀적으로 모니터링 대상 디렉토리를 탐색하여 후보 파일을 찾는 함수.
 * - ssu_cleanupd.config와 ssu_cleanupd.log 파일은 제외
 * - 옵션에 따라 제외할 경로(excludePaths)와 파일 확장자(extension) 필터를 적용
 * - 정규 파일이면 후보 리스트에 업데이트.
 */
void scanDirectoryForCandidates(const char* monitorPath, const char* excludePaths, const char* extensions, int mode) {
    struct dirent** namelist;
    int n = scandir(monitorPath, &namelist, NULL, alphasort);
    if (n < 0) return;

    for (int i = 0; i < n; i++) {
        struct dirent* d = namelist[i];
        // 현재 디렉토리 및 상위 디렉토리는 제외
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0) {
            free(d);
            continue;
        }
        // ssu_cleanupd.config, ssu_cleanupd.log 파일은 스캔 대상에서 제외
        if (strcmp(d->d_name, "ssu_cleanupd.config") == 0 || strcmp(d->d_name, "ssu_cleanupd.log") == 0) {
            free(d);
            continue;
        }
        char pathBuf[MAX_PATH];
        SAFE_SNPRINTF(pathBuf, sizeof(pathBuf), "%s/%s", monitorPath, d->d_name);
        struct stat st;
        if (stat(pathBuf, &st) == 0) {
            // excludePaths 옵션 처리
            if (strcmp(excludePaths, "none") != 0) {
                char excludesCopy[1024];
                strncpy(excludesCopy, excludePaths, sizeof(excludesCopy));
                excludesCopy[sizeof(excludesCopy) - 1] = '\0';
                char* exToken = strtok(excludesCopy, ",");
                int skipFlag = 0;
                while (exToken != NULL) {
                    if (isSubdirectory(exToken, pathBuf)) {
                        skipFlag = 1;
                        break;
                    }
                    exToken = strtok(NULL, ",");
                }
                if (skipFlag) {
                    free(d);
                    continue;
                }
            }
            // 디렉토리인 경우 재귀 호출로 하위 디렉토리 탐색
            if (S_ISDIR(st.st_mode)) {
                scanDirectoryForCandidates(pathBuf, excludePaths, extensions, mode);
            }
            // 정규 파일인 경우
            else if (S_ISREG(st.st_mode)) {
                char ext[32];
                getExtension(d->d_name, ext, sizeof(ext));
                // 확장자를 소문자로 변환
                for (int j = 0; ext[j]; j++) {
                    ext[j] = tolower(ext[j]);
                }
                // extensions 옵션이 "all"이 아니면 해당 확장자만 통과시킴
                if (strcmp(extensions, "all") != 0) {
                    int found = 0;
                    char extCopy[256];
                    strncpy(extCopy, extensions, sizeof(extCopy));
                    extCopy[sizeof(extCopy) - 1] = '\0';
                    for (int j = 0; extCopy[j]; j++) {
                        extCopy[j] = tolower(extCopy[j]);
                    }
                    char* extToken = strtok(extCopy, ",");
                    while (extToken != NULL) {
                        if (strcmp(ext, extToken) == 0) {
                            found = 1;
                            break;
                        }
                        extToken = strtok(NULL, ",");
                    }
                    if (!found) {
                        free(d);
                        continue;
                    }
                }
                // swp 확장자 파일은 제외
                if (strcmp(ext, "swp") == 0) {
                    free(d);
                    continue;
                }
                // 후보 리스트 업데이트: 파일명, 전체 경로, 수정 시간 정보 전달
                updateCandidate(d->d_name, pathBuf, st.st_mtime, mode);
            }
        }
        free(d);
    }
    free(namelist);
}

/*
 * 후보 리스트에 저장된 파일들을 대상으로 파일을 정리(복사)하고 로그를 기록하는 함수.
 * 대상 디렉토리 내에 각 파일의 확장자별 서브디렉토리를 생성한 후 복사하며,
 * 기존 파일이 있고 수정시간이 같으면 복사하지 않음.
 */
void arrangeCandidates(const char* arrangedPath, const char* logPath, const char* monitorPath, pid_t daemonPid) {
    CandidateNode* curr = candidateList;
    while (curr != NULL) {
        // mode 3 (중복 처리)일 경우 duplicate 플래그가 1이면 정리하지 않음
        if (curr->duplicate == 1) {
            curr = curr->next;
            continue;
        }
        // 대상 디렉토리: arrangedPath/<확장자> 형식으로 구성
        char ext[32];
        getExtension(curr->filename, ext, sizeof(ext));
        for (int j = 0; ext[j]; j++) {
            ext[j] = tolower(ext[j]);
        }
        char extDir[MAX_PATH];
        SAFE_SNPRINTF(extDir, sizeof(extDir), "%s/%s", arrangedPath, ext);
        mkdir(extDir, 0755);  // 디렉토리가 이미 존재하면 무시됨
        char dstBuf[MAX_PATH];
        SAFE_SNPRINTF(dstBuf, sizeof(dstBuf), "%s/%s", extDir, curr->filename);

        int shouldCopy = 1;
        struct stat dstStat;
        if (stat(dstBuf, &dstStat) == 0) {
            // 대상 파일이 존재하면 수정 시간을 비교하여 변경 사항이 있는지 확인
            if (dstStat.st_mtime == curr->mtime) {
                shouldCopy = 0;
            }
        }
        // 변경 사항이 있는 경우에만 파일 복사 및 로그 기록
        if (shouldCopy) {
            if (copyFile(curr->fullPath, dstBuf) == 0) {
                writeLog(logPath, daemonPid, curr->fullPath, dstBuf, monitorPath);
            }
        }
        curr = curr->next;
    }
}

/*
 * 후보 파일 리스트에 할당된 메모리를 해제하는 함수.
 * 메모리 누수를 방지하기 위해 리스트의 모든 노드를 free하고, candidateList 포인터를 NULL로 설정.
 */
void freeCandidateList() {
    CandidateNode* curr = candidateList;
    while (curr != NULL) {
        CandidateNode* next = curr->next;
        free(curr);
        curr = next;
    }
    candidateList = NULL;
}

/*
 * 지정한 모니터링 디렉토리(monitorPath)에 ssu_cleanupd.config 파일을 생성하고,
 * 데몬 프로세스의 설정값(모니터링 경로, pid, 시작 시간, 출력 경로, 주기, 로그 옵션 등)을 기록.
 * 파일이 이미 존재하면 새로 생성하지 않는다.
 */
void createConfigFile(const char* monitorPath, const char* arrangedPath, pid_t pid, int timeInterval, const char* maxLogLines, const char* excludePaths, const char* extensions, int mode) {
    char configPath[MAX_PATH];
    SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", monitorPath);

    FILE* fp = fopen(configPath, "r");
    if (!fp) {
        fp = fopen(configPath, "w");
        if (!fp) {
            perror("config file create error");
            return;
        }
        // 현재 시간을 start_time으로 기록
        time_t t = time(NULL);
        struct tm* tmInfo = localtime(&t);
        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", tmInfo);

        // 각 설정 값을 파일에 출력
        fprintf(fp, "monitoring_path : %s\n", monitorPath);
        fprintf(fp, "pid : %d\n", pid);
        fprintf(fp, "start_time : %s\n", timeBuf);
        fprintf(fp, "output_path : %s\n", arrangedPath);
        fprintf(fp, "time_interval : %d\n", timeInterval);
        fprintf(fp, "max_log_lines : %s\n", maxLogLines);
        fprintf(fp, "exclude_path : %s\n", excludePaths);
        fprintf(fp, "extension : %s\n", extensions);
        fprintf(fp, "mode : %d\n", mode);
        fclose(fp);
    }
    else {
        fclose(fp);
    }
}

/*
 * 모니터링 디렉토리 내에 ssu_cleanupd.log 파일이 존재하지 않으면 생성하는 함수.
 */
void createLogFile(const char* monitorPath) {
    char logPath[MAX_PATH];
    SAFE_SNPRINTF(logPath, sizeof(logPath), "%s/ssu_cleanupd.log", monitorPath);

    FILE* fp = fopen(logPath, "r");
    if (!fp) {
        fp = fopen(logPath, "w");
        if (!fp) {
            perror("log file create error");
            return;
        }
        fclose(fp);
    }
    else {
        fclose(fp);
    }
}



/*
 * 기존 ssu_cleanupd.config 파일을 업데이트하는 함수.
 * 파일 락을 사용하여 동시 접근 문제를 해결하며, modify 명령 시 기존 start_time 값을 유지.
 */
void updateConfigFile(const char* monitorPath, const char* arrangedPath, pid_t pid,
    int timeInterval, const char* maxLogLines, const char* excludePaths,
    const char* extensions, int mode, const char* existingStartTime) {
    char configPath[MAX_PATH];
    SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", monitorPath);

    int fd = open(configPath, O_RDWR);
    if (fd < 0) {
        perror("open config file error");
        return;
    }

    // 파일 락 설정: 쓰기 락을 획득하여 동시 접근 방지
    struct flock lock;
    lock.l_type = F_WRLCK;  // 쓰기 락
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        perror("fcntl lock error");
        close(fd);
        return;
    }

    // 기존 start_time 값을 유지하거나, 없으면 현재 시간으로 설정
    char startTimeStr[64];
    if (existingStartTime != NULL && strlen(existingStartTime) > 0) {
        strncpy(startTimeStr, existingStartTime, sizeof(startTimeStr));
    }
    else {
        time_t t = time(NULL);
        struct tm* tmInfo = localtime(&t);
        strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S", tmInfo);
    }

    char buffer[1024];
    SAFE_SNPRINTF(buffer, sizeof(buffer),
        "monitoring_path : %s\n"
        "pid : %d\n"
        "start_time : %s\n"
        "output_path : %s\n"
        "time_interval : %d\n"
        "max_log_lines : %s\n"
        "exclude_path : %s\n"
        "extension : %s\n"
        "mode : %d\n",
        monitorPath, pid, startTimeStr, arrangedPath, timeInterval,
        maxLogLines, excludePaths, extensions, mode);

    // 파일 내용을 새로 기록: 기존 내용을 삭제 후 새로운 내용 기록
    ftruncate(fd, 0);
    lseek(fd, 0, SEEK_SET);
    write(fd, buffer, strlen(buffer));

    // 락 해제
    lock.l_type = F_UNLCK;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        perror("fcntl unlock error");
    }
    close(fd);
}

/*
 * 파일 락을 사용하여 ssu_cleanupd.config 파일을 읽고, 내용을 Config 구조체에 저장하는 함수.
 */
int readConfigFileWithLock(const char* monitorPath, Config* config) {
    char configPath[MAX_PATH];
    SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", monitorPath);

    FILE* fp = fopen(configPath, "r");
    if (!fp) {
        perror("open config file for reading error");
        return -1;
    }

    int fd = fileno(fp);
    struct flock lock;
    lock.l_type = F_RDLCK;  // 읽기 락
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        perror("fcntl lock error in readConfig");
        fclose(fp);
        return -1;
    }

    // 기본값 설정 (파일에 값이 없으면)
    memset(config, 0, sizeof(Config));
    strcpy(config->max_log_lines, "none");
    strcpy(config->exclude_path, "none");
    strcpy(config->extension, "all");
    config->time_interval = 10;
    config->mode = 1;

    char buffer[MAX_LINE];
    // 파일을 한 줄씩 읽으며 구분자를 기준으로 파싱
    while (fgets(buffer, sizeof(buffer), fp)) {
        // monitoring_path 파싱
        if (strncmp(buffer, "monitoring_path : ", 18) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++; // 콜론 이후 공백 건너뛰기
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->monitoring_path, p, MAX_PATH);
            }
        }
        // output_path 파싱
        else if (strncmp(buffer, "output_path : ", 14) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->output_path, p, MAX_PATH);
            }
        }
        // time_interval 파싱
        else if (strncmp(buffer, "time_interval : ", 16) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                config->time_interval = atoi(p);
            }
        }
        // max_log_lines 파싱
        else if (strncmp(buffer, "max_log_lines : ", 16) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->max_log_lines, p, sizeof(config->max_log_lines));
            }
        }
        // exclude_path 파싱
        else if (strncmp(buffer, "exclude_path : ", 15) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->exclude_path, p, sizeof(config->exclude_path));
            }
        }
        // extension 파싱
        else if (strncmp(buffer, "extension : ", 12) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->extension, p, sizeof(config->extension));
            }
        }
        // mode 파싱
        else if (strncmp(buffer, "mode : ", 7) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                config->mode = atoi(p);
            }
        }
        // start_time 파싱
        else if (strncmp(buffer, "start_time : ", 13) == 0) {
            char* p = strchr(buffer, ':');
            if (p != NULL) {
                p++;
                while (*p && isspace((unsigned char)*p))
                    p++;
                p[strcspn(p, "\n")] = '\0';
                strncpy(config->start_time, p, sizeof(config->start_time));
            }
        }
    }
    // 락 해제
    lock.l_type = F_UNLCK;
    if (fcntl(fd, F_SETLKW, &lock) == -1) {
        perror("fcntl unlock error in readConfig");
    }
    fclose(fp);
    return 0;
}

/*
 * 데몬 프로세스의 메인 루프 함수.
 * 주기적으로 설정 파일을 읽고(옵션 업데이트) 모니터링 대상 디렉토리를 스캔하여 후보 파일을 정리하며,
 * 파일을 복사하고 로그를 기록하는 역할을 수행.
 */
void daemonProcessLoop(const char* absPath) {
    pid_t daemonPid = getpid();
    char logPath[MAX_PATH];
    char arrangedPath[MAX_PATH];
    Config config;
    int timeInterval = 10; // 기본 모니터링 주기

    // 초기 기본 출력 디렉토리: config 파일 읽기에 실패하면 absPath에 "_arranged" 
    SAFE_SNPRINTF(arrangedPath, sizeof(arrangedPath), "%s_arranged", absPath);
    SAFE_SNPRINTF(logPath, sizeof(logPath), "%s/ssu_cleanupd.log", absPath);

    while (1) {
        // 설정 파일을 락을 걸고 읽어서 설정 업데이트
        if (readConfigFileWithLock(absPath, &config) == 0) {
            timeInterval = config.time_interval;
            strncpy(arrangedPath, config.output_path, MAX_PATH);
            arrangedPath[MAX_PATH - 1] = '\0';
            
        }
        // 매 사이클마다 후보 리스트 초기화 후 디렉토리 스캔 및 파일 정리
        freeCandidateList();
        candidateList = NULL;
        scanDirectoryForCandidates(absPath, config.exclude_path, config.extension, config.mode);
        arrangeCandidates(arrangedPath, logPath, absPath, daemonPid);
        sleep(timeInterval);

        
    }
}

/*
 * 사용자의 홈 디렉토리 내에 .ssu_cleanupd 디렉토리와 current_daemon_list 파일을
 * 생성하여 데몬 프로세스 관련 정보를 저장할 공간을 초기화하는 함수.
 */
void initHomeDir() {
    char* home = getenv("HOME");
    if (home == NULL) {
        fprintf(stderr, "HOME 환경변수가 설정되어 있지 않습니다.\n");
        exit(EXIT_FAILURE);
    }

    char cleanupDir[MAX_PATH];
    SAFE_SNPRINTF(cleanupDir, MAX_PATH, "%s/.ssu_cleanupd", home);

    struct stat st;
    if (stat(cleanupDir, &st) == -1) {
        if (mkdir(cleanupDir, 0755) == -1) {
            perror("디렉토리 생성 실패");
            exit(EXIT_FAILURE);
        }
    }

    // daemonList 파일 생성: 데몬 프로세스들의 정보를 기록할 파일
    char daemonListFile[MAX_PATH];
    SAFE_SNPRINTF(daemonListFile, MAX_PATH, "%s/current_daemon_list", cleanupDir);
    FILE* fp = fopen(daemonListFile, "r");
    if (fp == NULL) {
        fp = fopen(daemonListFile, "w");
        if (fp == NULL) {
            perror("파일 생성 실패");
            exit(EXIT_FAILURE);
        }
    }
    if (fp) {
        fclose(fp);
    }
}

/*
 * 현재 모니터링 중인 데몬 프로세스들의 정보를 출력하는 함수.
 * 사용자가 특정 데몬을 선택하면 해당 데몬의 ssu_cleanupd.config와 ssu_cleanupd.log 파일 내용을 출력.
 */
void show_command() {
    char daemonListFile[MAX_PATH];
    char buffer[MAX_LINE];
    char* daemonPaths[100];  // 최대 100개의 데몬 모니터링 경로 저장
    int count = 0;

    // HOME 환경변수를 기반으로 daemonList 파일 경로 구성
    char* home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "HOME 환경변수가 설정되어 있지 않습니다.\n");
        return;
    }
    SAFE_SNPRINTF(daemonListFile, sizeof(daemonListFile), "%s/.ssu_cleanupd/current_daemon_list", home);

    FILE* fp = fopen(daemonListFile, "r");
    if (!fp) {
        printf("No daemon processes are currently being monitored..\n");
        return;
    }

    // 파일의 각 줄(절대경로)을 읽어 배열에 저장
    while (fgets(buffer, sizeof(buffer), fp) != NULL && count < 100) {
        buffer[strcspn(buffer, "\n")] = '\0'; // 개행 문자 제거
        if (strlen(buffer) > 0) {
            daemonPaths[count] = strdup(buffer);
            count++;
        }
    }
    fclose(fp);

    // 사용자 입력을 받아 데몬 프로세스의 정보를 출력
    while (1) {
        // 데몬 목록 출력
        printf("Current working daemon process list\n");
        printf("\n0. exit\n");
        for (int i = 0; i < count; i++) {
            printf("%d. %s\n", i + 1, daemonPaths[i]);
        }
        printf("\nSelect one to see process info : ");

        int choice = -1;
        if (scanf("%d", &choice) != 1) {
            // 잘못된 입력일 경우 버퍼를 초기화하고 재입력 요청
            printf("Please check your input is valid\n");
            while (getchar() != '\n'); // 입력 버퍼 정리
            continue;
        }
        while (getchar() != '\n');  // 남은 개행문자 제거

        // 입력한 번호가 유효 범위인지 확인
        if (choice < 0 || choice > count) {
            printf("Please check your input is valid\n");
            continue;
        }

        if (choice == 0) {
            break; // 선택 0인 경우 종료
        }

        // 선택한 데몬의 경로
        char* selectedPath = daemonPaths[choice - 1];

        // 해당 데몬의 config와 log 파일 경로 구성
        char configPath[MAX_PATH], logPath[MAX_PATH];
        SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", selectedPath);
        SAFE_SNPRINTF(logPath, sizeof(logPath), "%s/ssu_cleanupd.log", selectedPath);

        // config 상세 내용 출력 (내용 전체 출력)
        printf("\n1. config detail\n");
        FILE* fconfig = fopen(configPath, "r");
        if (!fconfig) {
            printf("Cannot open config file: %s\n", configPath);
        }
        else {
            while (fgets(buffer, sizeof(buffer), fconfig)) {
                printf("%s", buffer);
            }
            fclose(fconfig);
        }

        // log 파일 최신 내용 출력 (최근 최대 maxLog 줄)
        printf("\n2. log detail\n");
        Config config;
        int maxLog = 10;  // 기본적으로 최신 10줄 출력
        if (readConfigFileWithLock(selectedPath, &config) == 0) {
            if (strcmp(config.max_log_lines, "none") != 0) {
                int parsed = atoi(config.max_log_lines);
                if (parsed > 0)
                    maxLog = parsed;
            }
        }

        FILE* flog = fopen(logPath, "r");
        if (!flog) {
            printf("Cannot open log file: %s\n", logPath);
        }
        else {
            char* logLines[1000];
            int logCount = 0;
            while (fgets(buffer, sizeof(buffer), flog) != NULL && logCount < 1000) {
                logLines[logCount] = strdup(buffer);
                logCount++;
            }
            fclose(flog);
            // 최근 maxLog 줄만 출력하도록 시작 인덱스 계산
            int start = (logCount > maxLog) ? (logCount - maxLog) : 0;
            for (int i = start; i < logCount; i++) {
                printf("%s", logLines[i]);
                free(logLines[i]);
            }
            // 나머지 동적할당된 메모리 해제
            for (int i = 0; i < start; i++) {
                free(logLines[i]);
            }
        }
        printf("\n");

        break;
    } // end of main while

    // 동적으로 할당된 데몬 경로 배열 메모리 해제
    for (int i = 0; i < count; i++) {
        free(daemonPaths[i]);
    }
}

/*
 * add 명령어 처리 함수.
 * 지정한 디렉토리를 모니터링 대상으로 추가하며, 옵션에 따라 출력 경로, 주기, 로그 등 추가 설정을 파싱한다.
 * 유효성 검사 후 데몬 프로세스를 fork하여 데몬 실행.
 */
void add_command(char* arguments) {
    if (arguments == NULL) {
        printf("Usage: add <DIR_PATH> [OPTIONS]...\n");
        return;
    }

    char* tokens[100];
    int tokenCount = 0;
    char* token = strtok(arguments, " ");
    while (token != NULL && tokenCount < 100) {
        tokens[tokenCount++] = token;
        token = strtok(NULL, " ");
    }

    if (tokenCount < 1) {
        printf("Usage: add <DIR_PATH> [OPTIONS]...\n");
        return;
    }
    // 첫번째 토큰은 모니터링 디렉토리 경로
    char* dirPath = tokens[0];
    char absPath[MAX_PATH];
    if (!realpath(dirPath, absPath)) {
        perror("realpath error");
        return;
    }
    if (!isInsideHome(absPath)) {
        printf("<%s> is outside the home directory\n", absPath);
        return;
    }
    if (!isDirectory(absPath)) {
        printf("<%s> is not a directory or not accessible.\n", absPath);
        return;
    }
    // 이미 모니터링 중인 경로와 중복 또는 포함되는지 검사
    DaemonNode* cur = daemonList;
    while (cur) {
        if (isSubdirectory(cur->dirPath, absPath) || isSubdirectory(absPath, cur->dirPath)) {
            printf("Error: The specified directory <%s> is already monitored or overlaps with <%s>.\n", absPath, cur->dirPath);
            return;
        }
        cur = cur->next;
    }

    // 기본 옵션 값 설정
    int timeInterval = 10;
    char outputPath[MAX_PATH] = "";
    char maxLogLines[64];
    strcpy(maxLogLines, "none");
    char excludePaths[1024];
    strcpy(excludePaths, "none");
    char extensions[256];
    strcpy(extensions, "all");
    int mode = 1;
    global_max_log_lines = -1;

    // 옵션 파싱 (-d, -i, -l, -x, -e, -m)
    for (int i = 1; i < tokenCount; i++) {
        if (strcmp(tokens[i], "-d") == 0) {
            // -d 옵션: 출력 디렉토리(outputPath) 지정
            if (i + 1 >= tokenCount) {
                // 출력 경로 인자가 없으면 에러 메시지 출력
                printf("Error: -d option requires an output path argument.\n");
                return;
            }
            i++; // 다음 토큰에 출력 경로 값이 있음
            // 만약 경로가 절대경로가 아니라면 현재 작업 디렉토리(cwd)와 결합하여 절대경로로 변환
            if (tokens[i][0] != '/') {
                char cwd[MAX_PATH];
                if (getcwd(cwd, MAX_PATH) != NULL) {
                    SAFE_SNPRINTF(outputPath, sizeof(outputPath), "%s/%s", cwd, tokens[i]);
                }
                else {
                    // getcwd 실패 시 그냥 입력 토큰을 그대로 복사
                    strncpy(outputPath, tokens[i], sizeof(outputPath));
                }
            }
            else {
                // 입력 토큰이 이미 절대경로이면 그대로 복사
                strncpy(outputPath, tokens[i], sizeof(outputPath));
            }
            outputPath[sizeof(outputPath) - 1] = '\0';
            // 출력 경로가 실제 존재하는 디렉토리인지 확인
            if (!isDirectory(outputPath)) {
                printf("Error: Output directory <%s> does not exist, is not a directory, or is not accessible.\n", outputPath);
                return;
            }
            // 출력 경로가 홈 디렉토리 내부인지 확인
            if (!isInsideHome(outputPath)) {
                printf("Error: Output directory <%s> is outside the home directory.\n", outputPath);
                return;
            }
            // 출력 디렉토리가 모니터링 대상 디렉토리의 하위에 있으면 안됨
            if (isSubdirectory(absPath, outputPath)) {
                printf("Error: Output directory <%s> should not be a subdirectory of the monitored directory <%s>.\n", outputPath, absPath);
                return;
            }
        }
        else if (strcmp(tokens[i], "-i") == 0) {
            // -i 옵션: 모니터링 주기(timeInterval)를 지정
            if (i + 1 < tokenCount) {
                i++; // 다음 토큰에 주기 값이 있음
                // 입력된 주기 값이 모든 문자가 숫자인지 체크
                for (int j = 0; tokens[i][j] != '\0'; j++) {
                    if (tokens[i][j] < '0' || tokens[i][j] > '9') {
                        printf("Error: Time interval (-i) must be a natural number.\n");
                        return;
                    }
                }
                timeInterval = atoi(tokens[i]); // 문자열을 정수로 변환
                if (timeInterval <= 0) {
                    printf("Error: Time interval (-i) must be greater than 0.\n");
                    return;
                }
            }
        }
        else if (strcmp(tokens[i], "-l") == 0) {
            // -l 옵션: 로그 최대 줄 수(maxLogLines) 지정
            if (i + 1 < tokenCount) {
                i++; // 다음 토큰에 로그 줄 수가 있음
                // 입력된 로그 줄 수가 숫자로만 구성되었는지 확인
                for (int j = 0; tokens[i][j] != '\0'; j++) {
                    if (tokens[i][j] < '0' || tokens[i][j] > '9') {
                        printf("Error: Max log lines (-l) must be a natural number.\n");
                        return;
                    }
                }
                int parsedValue = atoi(tokens[i]);
                if (parsedValue <= 0) {
                    printf("Error: Max log lines (-l) must be greater than 0.\n");
                    return;
                }
                strncpy(maxLogLines, tokens[i], sizeof(maxLogLines));
                maxLogLines[sizeof(maxLogLines) - 1] = '\0';
                global_max_log_lines = parsedValue;
            }
        }
        else if (strcmp(tokens[i], "-x") == 0) {
            // -x 옵션: 제외할 디렉토리 리스트(excludePaths)를 지정 (여러 개 가능)
            excludePaths[0] = '\0';
            int first = 1;
            // 옵션 인자들이 '-'로 시작하지 않을 때까지 반복하여 여러 경로 입력을 처리
            while (i + 1 < tokenCount && tokens[i + 1][0] != '-') {
                i++; // 제외 경로가 담긴 토큰을 읽음
                char exAbs[MAX_PATH];
                // 입력된 경로가 절대경로가 아니면 현재 작업 디렉토리를 기준으로 절대경로 변환
                if (tokens[i][0] != '/') {
                    char cwd[MAX_PATH];
                    if (getcwd(cwd, MAX_PATH) != NULL) {
                        char combined[MAX_PATH];
                        SAFE_SNPRINTF(combined, sizeof(combined), "%s/%s", cwd, tokens[i]);
                        if (!realpath(combined, exAbs)) {
                            printf("Error: Exclude path <%s> is invalid.\n", tokens[i]);
                            return;
                        }
                    }
                    else {
                        printf("Error: Unable to get current working directory.\n");
                        return;
                    }
                }
                else {
                    if (!realpath(tokens[i], exAbs)) {
                        printf("Error: Exclude path <%s> is invalid.\n", tokens[i]);
                        return;
                    }
                }
                // 입력된 경로가 실제 디렉토리인지 확인
                if (!isDirectory(exAbs)) {
                    printf("Error: Exclude path <%s> is not a directory or not accessible.\n", exAbs);
                    return;
                }
                // 홈 디렉토리 내부에 있는지 확인
                if (!isInsideHome(exAbs)) {
                    printf("Error: Exclude path <%s> is outside the home directory.\n", exAbs);
                    return;
                }
                // 제외할 경로가 모니터링 대상 디렉토리의 하위 디렉토리인지 확인
                size_t absLen = strlen(absPath);
                if (strncmp(exAbs, absPath, absLen) != 0 || (exAbs[absLen] != '/' && exAbs[absLen] != '\0')) {
                    printf("Error: Exclude path <%s> is not a subdirectory of the monitored directory <%s>.\n", exAbs, absPath);
                    return;
                }
                // 이미 입력된 제외 경로들과의 중복 및 포함 관계를 검사
                if (excludePaths[0] != '\0') {
                    char tempEx[1024];
                    strncpy(tempEx, excludePaths, sizeof(tempEx));
                    tempEx[sizeof(tempEx) - 1] = '\0';
                    char* saveptr;
                    char* prevPath = strtok_r(tempEx, ",", &saveptr);
                    while (prevPath != NULL) {
                        size_t len = strlen(prevPath);
                        if (strncmp(prevPath, exAbs, len) == 0) {
                            if (exAbs[len] == '/' || exAbs[len] == '\0' ||
                                prevPath[len] == '/' || prevPath[len] == '\0') {
                                printf("Error: Exclude paths <%s> and <%s> overlap or are identical.\n", prevPath, exAbs);
                                return;
                            }
                        }
                        prevPath = strtok_r(NULL, ",", &saveptr);
                    }
                }
                // 첫 번째 제외 경로인지 확인 후 저장; 두 번째부터는 콤마로 구분하여 연결
                if (first) {
                    SAFE_SNPRINTF(excludePaths, sizeof(excludePaths), "%s", exAbs);
                    first = 0;
                }
                else {
                    strncat(excludePaths, ",", sizeof(excludePaths) - strlen(excludePaths) - 1);
                    strncat(excludePaths, exAbs, sizeof(excludePaths) - strlen(excludePaths) - 1);
                }
            }
            if (strlen(excludePaths) == 0)
                strcpy(excludePaths, "none");
        }
        else if (strcmp(tokens[i], "-e") == 0) {
            // -e 옵션: 정리 대상으로 할 파일 확장자(extension)를 지정 (여러 개 가능)
            extensions[0] = '\0';
            int first = 1;
            // 옵션 인자들이 '-'로 시작하지 않을 때까지 모두 읽어 들임
            while (i + 1 < tokenCount && tokens[i + 1][0] != '-') {
                i++;
                // 확장자 비교를 위해 모든 문자를 소문자로 변환
                for (int j = 0; tokens[i][j]; j++) {
                    tokens[i][j] = tolower(tokens[i][j]);
                }
                if (first) {
                    SAFE_SNPRINTF(extensions, sizeof(extensions), "%s", tokens[i]);
                    first = 0;
                }
                else {
                    strncat(extensions, ",", sizeof(extensions) - strlen(extensions) - 1);
                    strncat(extensions, tokens[i], sizeof(extensions) - strlen(extensions) - 1);
                }
            }
            // 만약 확장자 옵션이 비어있다면 기본값 "all"로 설정
            if (strlen(extensions) == 0)
                strcpy(extensions, "all");
        }
        else if (strcmp(tokens[i], "-m") == 0) {
            // -m 옵션: 정리 모드(mode)를 지정 (1: 최신, 2: 오래된, 3: 중복시 정리하지 않음)
            if (i + 1 < tokenCount) {
                i++;
                // 모드 값이 숫자로만 구성되어 있는지 확인
                for (int j = 0; tokens[i][j] != '\0'; j++) {
                    if (tokens[i][j] < '0' || tokens[i][j] > '9') {
                        printf("Error: Mode (-m) must be a natural number.\n");
                        return;
                    }
                }
                int modeVal = atoi(tokens[i]);
                if (modeVal < 1 || modeVal > 3) {
                    printf("Error: Mode (-m) must be in the range 1 to 3.\n");
                    return;
                }
                mode = modeVal;
            }
        }
        else {
            // 등록되지 않은 옵션이 입력되면 에러 처리
            printf("Error: Unknown option <%s>.\n", tokens[i]);
            return;
        }
    }

    // arrangedPath 결정: -d 옵션이 없으면 기본값은 absPath에 "_arranged"
    char arrangedPath[MAX_PATH];
    if (strlen(outputPath) == 0) {
        SAFE_SNPRINTF(arrangedPath, sizeof(arrangedPath), "%s_arranged", absPath);
        if (mkdir(arrangedPath, 0755) == -1 && errno != EEXIST) {
            perror("mkdir error for arranged folder");
            return;
        }
    }
    else {
        char temp[MAX_PATH];
        if (realpath(outputPath, temp) != NULL)
            strncpy(arrangedPath, temp, sizeof(arrangedPath));
        else
            strncpy(arrangedPath, outputPath, sizeof(arrangedPath));
        arrangedPath[sizeof(arrangedPath) - 1] = '\0';
    }

    // config 파일과 log 파일을 생성한 후, 데몬 프로세스 생성
    createConfigFile(absPath, arrangedPath, 0, timeInterval, maxLogLines, excludePaths, extensions, mode);
    createLogFile(absPath);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork error");
        return;
    }
    else if (pid == 0) {
        // 자식 프로세스: 데몬 프로세스로 전환
        if (setsid() < 0) {
            perror("setsid error");
            exit(EXIT_FAILURE);
        }
        chdir("/");
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO);
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd > STDERR_FILENO)
                close(fd);
        }
        daemonProcessLoop(absPath);
        exit(EXIT_SUCCESS);
    }
    else {
        // 부모 프로세스: 데몬 등록 및 config 파일 업데이트
        insertDaemonNode(absPath, pid);
        updateDaemonListFile(absPath); // 데몬 목록 파일에 기록
        char startTimeStr[64];
        time_t t = time(NULL);
        struct tm* tmInfo = localtime(&t);
        strftime(startTimeStr, sizeof(startTimeStr), "%Y-%m-%d %H:%M:%S", tmInfo);
        updateConfigFile(absPath, arrangedPath, pid, timeInterval, maxLogLines, excludePaths, extensions, mode, startTimeStr);
        
    }
}

/*
 * modify 명령어 처리 함수.
 * 모니터링 중인 디렉토리의 config 파일을 읽은 후, 사용자로부터 입력받은 옵션만 업데이트하여 재기록.
 */
void modify_command(char* arguments) {
    if (arguments == NULL) {
        printf("Usage: modify <DIR_PATH> [OPTION]...\n");
        return;
    }
    // 토큰화: 첫 토큰은 DIR_PATH, 이후 옵션들
    char* tokens[100];
    int tokenCount = 0;
    char* token = strtok(arguments, " ");
    while (token != NULL && tokenCount < 100) {
        tokens[tokenCount++] = token;
        token = strtok(NULL, " ");
    }
    if (tokenCount < 1) {
        printf("Usage: modify <DIR_PATH> [OPTION]...\n");
        return;
    }

    // 첫 번째 인자: 모니터링 디렉토리 경로
    char* dirPath = tokens[0];
    char absPath[MAX_PATH];
    if (!realpath(dirPath, absPath)) {
        perror("realpath error");
        return;
    }
    struct stat st;
    if (stat(absPath, &st) < 0 || !S_ISDIR(st.st_mode)) {
        printf("Error: %s does not exist, is not a directory, or is inaccessible.\n", absPath);
        return;
    }
    if (!isInsideHome(absPath)) {
        printf("%s is outside the home directory\n", absPath);
        return;
    }
    // 해당 경로가 현재 모니터링 중인지 검사
    DaemonNode* curr = daemonList;
    int found = 0;
    int pid = 0;
    char existingStartTime[64] = "";
    while (curr) {
        if (strcmp(curr->dirPath, absPath) == 0) {
            found = 1;
            pid = curr->pid;
            break;
        }
        curr = curr->next;
    }
    if (!found) {
        printf("Error: %s is not being monitored\n", absPath);
        return;
    }

    // 기존 config 파일에서 현재 설정값 읽어오기
    char configPath[MAX_PATH];
    SAFE_SNPRINTF(configPath, sizeof(configPath), "%s/ssu_cleanupd.config", absPath);
    char buf[MAX_LINE];
    char currentOutput[MAX_PATH] = "";
    int currentInterval = 0;
    char currentMaxLog[64] = "";
    char currentExclude[1024] = "";
    char currentExtension[256] = "";
    int currentMode = 0;
    FILE* fconfig = fopen(configPath, "r");
    if (fconfig) {
        while (fgets(buf, sizeof(buf), fconfig)) {
            if (strncmp(buf, "output_path : ", 14) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    strncpy(currentOutput, p, MAX_PATH);
                }
            }
            else if (strncmp(buf, "time_interval : ", 16) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    currentInterval = atoi(p);
                }
            }
            else if (strncmp(buf, "max_log_lines : ", 16) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    strncpy(currentMaxLog, p, sizeof(currentMaxLog));
                }
            }
            else if (strncmp(buf, "exclude_path : ", 15) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    strncpy(currentExclude, p, sizeof(currentExclude));
                }
            }
            else if (strncmp(buf, "extension : ", 12) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    strncpy(currentExtension, p, sizeof(currentExtension));
                }
            }
            else if (strncmp(buf, "mode : ", 7) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    currentMode = atoi(p);
                }
            }
            else if (strncmp(buf, "start_time : ", 13) == 0) {
                char* p = strchr(buf, ':');
                if (p != NULL) {
                    p++; while (*p && isspace((unsigned char)*p)) p++;
                    p[strcspn(p, "\n")] = '\0';
                    strncpy(existingStartTime, p, sizeof(existingStartTime));
                }
            }
        }
        fclose(fconfig);
    }
    else {
        printf("Error: Config file not found in %s\n", absPath);
        return;
    }

    // 초기에는 기존 config값으로 새로운 변수들을 설정
    char newOutput[MAX_PATH];
    strcpy(newOutput, currentOutput);
    int newInterval = currentInterval;
    char newMaxLog[64];
    strcpy(newMaxLog, currentMaxLog);
    char newExclude[1024];
    strcpy(newExclude, currentExclude);
    char newExtension[256];
    strcpy(newExtension, currentExtension);
    int newMode = currentMode;

    

    for (int i = 1; i < tokenCount; i++) {
        if (strcmp(tokens[i], "-d") == 0) {
            // -d 옵션: 출력 디렉토리를 새롭게 지정
            if (i + 1 >= tokenCount) {
                printf("Error: -d option requires an output path argument.\n");
                return;
            }
            i++;
            char candidate[MAX_PATH];
            if (tokens[i][0] != '/') {
                char cwd[MAX_PATH];
                if (getcwd(cwd, sizeof(cwd)) != NULL)
                    SAFE_SNPRINTF(candidate, sizeof(candidate), "%s/%s", cwd, tokens[i]);
                else {
                    printf("Error: Unable to get current working directory.\n");
                    return;
                }
            }
            else {
                strncpy(candidate, tokens[i], sizeof(candidate));
            }
            candidate[sizeof(candidate) - 1] = '\0';
            if (stat(candidate, &st) < 0 || !S_ISDIR(st.st_mode)) {
                printf("Error: Output directory <%s> does not exist, is not a directory, or is inaccessible.\n", candidate);
                return;
            }
            if (!isInsideHome(candidate)) {
                printf("Error: Output directory <%s> is outside the home directory.\n", candidate);
                return;
            }
            // 모니터링 대상 디렉토리와 동일하거나 포함 관계이면 안됨
            if (strncmp(candidate, absPath, strlen(absPath)) == 0 &&
                (candidate[strlen(absPath)] == '/' || candidate[strlen(absPath)] == '\0')) {
                printf("Error: Output directory <%s> should not be a subdirectory of the monitored directory <%s>.\n", candidate, absPath);
                return;
            }
            strcpy(newOutput, candidate);
        }
        else if (strcmp(tokens[i], "-i") == 0) {
            // -i 옵션: 모니터링 주기 업데이트
            if (i + 1 >= tokenCount) {
                printf("Error: -i option requires a time interval argument.\n");
                return;
            }
            i++;
            for (int j = 0; tokens[i][j] != '\0'; j++) {
                if (!isdigit(tokens[i][j])) {
                    printf("Error: Time interval (-i) must be a natural number.\n");
                    return;
                }
            }
            newInterval = atoi(tokens[i]);
            if (newInterval <= 0) {
                printf("Error: Time interval (-i) must be greater than 0.\n");
                return;
            }
        }
        else if (strcmp(tokens[i], "-l") == 0) {
            // -l 옵션: 로그 최대 줄 수 업데이트
            if (i + 1 >= tokenCount) {
                printf("Error: -l option requires a max log lines argument.\n");
                return;
            }
            i++;
            for (int j = 0; tokens[i][j] != '\0'; j++) {
                if (!isdigit(tokens[i][j])) {
                    printf("Error: Max log lines (-l) must be a natural number.\n");
                    return;
                }
            }
            int parsedValue = atoi(tokens[i]);
            if (parsedValue <= 0) {
                printf("Error: Max log lines (-l) must be greater than 0.\n");
                return;
            }
            strncpy(newMaxLog, tokens[i], sizeof(newMaxLog));
            newMaxLog[sizeof(newMaxLog) - 1] = '\0';

            int newMax = atoi(newMaxLog);
            char logFilePath[MAX_PATH];
            SAFE_SNPRINTF(logFilePath, sizeof(logFilePath), "%s/ssu_cleanupd.log", absPath);
            trimLogFile(logFilePath, newMax);
            
        }
        else if (strcmp(tokens[i], "-x") == 0) {
            // -x 옵션: 제외할 경로 리스트 업데이트
            newExclude[0] = '\0';
            int first = 1;
            // 여러 경로가 입력될 수 있으므로 계속 읽음
            while (i + 1 < tokenCount && tokens[i + 1][0] != '-') {
                i++;
                char candidate[MAX_PATH];
                if (tokens[i][0] != '/') {
                    char cwd[MAX_PATH];
                    if (getcwd(cwd, sizeof(cwd)) != NULL) {
                        char combined[MAX_PATH];
                        SAFE_SNPRINTF(combined, sizeof(combined), "%s/%s", cwd, tokens[i]);
                        if (!realpath(combined, candidate)) {
                            printf("Error: Exclude path <%s> is invalid.\n", tokens[i]);
                            return;
                        }
                    }
                    else {
                        printf("Error: Unable to get current working directory.\n");
                        return;
                    }
                }
                else {
                    if (!realpath(tokens[i], candidate)) {
                        printf("Error: Exclude path <%s> is invalid.\n", tokens[i]);
                        return;
                    }
                }
                if (stat(candidate, &st) < 0 || !S_ISDIR(st.st_mode)) {
                    printf("Error: Exclude path <%s> is not a directory or not accessible.\n", candidate);
                    return;
                }
                if (!isInsideHome(candidate)) {
                    printf("Error: Exclude path <%s> is outside the home directory.\n", candidate);
                    return;
                }
                size_t absLen = strlen(absPath);
                if (strncmp(candidate, absPath, absLen) != 0 ||
                    (candidate[absLen] != '/' && candidate[absLen] != '\0')) {
                    printf("Error: Exclude path <%s> is not a subdirectory of the monitored directory <%s>.\n", candidate, absPath);
                    return;
                }
                // 중복 또는 포함 관계를 확인하여 겹침이 있으면 에러 처리
                if (newExclude[0] != '\0') {
                    char tempEx[1024];
                    strncpy(tempEx, newExclude, sizeof(tempEx));
                    tempEx[sizeof(tempEx) - 1] = '\0';
                    char* saveptr;
                    char* prevPath = strtok_r(tempEx, ",", &saveptr);
                    while (prevPath != NULL) {
                        size_t len = strlen(prevPath);
                        if (strncmp(prevPath, candidate, len) == 0) {
                            if (candidate[len] == '/' || candidate[len] == '\0' ||
                                prevPath[len] == '/' || prevPath[len] == '\0') {
                                printf("Error: Exclude paths <%s> and <%s> overlap or are identical.\n", prevPath, candidate);
                                return;
                            }
                        }
                        prevPath = strtok_r(NULL, ",", &saveptr);
                    }
                }
                if (first) {
                    strcpy(newExclude, candidate);
                    first = 0;
                }
                else {
                    strncat(newExclude, ",", sizeof(newExclude) - strlen(newExclude) - 1);
                    strncat(newExclude, candidate, sizeof(newExclude) - strlen(newExclude) - 1);
                }
            }
            if (strlen(newExclude) == 0)
                strcpy(newExclude, "none");
        }
        else if (strcmp(tokens[i], "-e") == 0) {
            // -e 옵션: 파일 확장자 옵션 업데이트
            newExtension[0] = '\0';
            int first = 1;
            while (i + 1 < tokenCount && tokens[i + 1][0] != '-') {
                i++;
                for (int j = 0; tokens[i][j]; j++) {
                    tokens[i][j] = tolower(tokens[i][j]);
                }
                if (first) {
                    strcpy(newExtension, tokens[i]);
                    first = 0;
                }
                else {
                    strncat(newExtension, ",", sizeof(newExtension) - strlen(newExtension) - 1);
                    strncat(newExtension, tokens[i], sizeof(newExtension) - strlen(newExtension) - 1);
                }
            }
            if (strlen(newExtension) == 0)
                strcpy(newExtension, "all");
        }
        else if (strcmp(tokens[i], "-m") == 0) {
            // -m 옵션: 정리 모드(mode) 업데이트
            if (i + 1 < tokenCount) {
                i++;
                for (int j = 0; tokens[i][j] != '\0'; j++) {
                    if (!isdigit(tokens[i][j])) {
                        printf("Error: Mode (-m) must be a natural number.\n");
                        return;
                    }
                }
                int modeVal = atoi(tokens[i]);
                if (modeVal < 1 || modeVal > 3) {
                    printf("Error: Mode (-m) must be in the range 1 to 3.\n");
                    return;
                }
                newMode = modeVal;
            }
        }
        else {
            printf("Error: Unknown option <%s>.\n", tokens[i]);
            return;
        }

    }

    // 파일 락을 사용하여 기존 config 파일을 업데이트 (modify 시 기존 start_time 유지)
    updateConfigFile(absPath, newOutput, pid, newInterval, newMaxLog, newExclude, newExtension, newMode, existingStartTime);
    
    
  
}

/*
 * remove 명령어 처리 함수.
 * 지정한 모니터링 디렉토리의 데몬 프로세스를 종료하고, daemonList와 current_daemon_list 파일에서 제거한다.
 */
void remove_command(char* arguments) {
    if (arguments == NULL) {
        printf("Usage: remove <DIR_PATH>\n");
        return;
    }

    // 첫 번째 인자만 사용 (공백 이후는 무시)
    char* dirArg = strtok(arguments, " ");
    if (dirArg == NULL) {
        printf("Usage: remove <DIR_PATH>\n");
        return;
    }

    // 절대 경로로 변환
    char absPath[MAX_PATH];
    if (!realpath(dirArg, absPath)) {
        perror("realpath error");
        return;
    }

    // 홈 디렉토리 내부인지 확인
    if (!isInsideHome(absPath)) {
        printf("%s is outside the home directory\n", absPath);
        return;
    }

    // daemonList(링크드 리스트)에서 해당 절대경로를 찾음
    DaemonNode* prev = NULL, * curr = daemonList;
    while (curr != NULL) {
        if (strcmp(curr->dirPath, absPath) == 0)
            break;
        prev = curr;
        curr = curr->next;
    }
    if (curr == NULL) {
        // 모니터링 중인 데몬 목록에 해당 경로가 없으면 에러 출력
        printf("Error: %s is not being monitored\n", absPath);
        return;
    }

    // 해당 데몬 프로세스 종료 시도 (SIGTERM 시그널 전송)
    if (kill(curr->pid, SIGTERM) < 0) {
        perror("kill error");
        // 필요시 SIGKILL 등 추가 조치 가능
    }

    // 링크드 리스트에서 해당 노드 제거
    if (prev == NULL) {
        daemonList = curr->next;
    }
    else {
        prev->next = curr->next;
    }
    free(curr);

    // current_daemon_list 파일에서 해당 경로 제거 후 갱신
    updateDaemonListFileAfterRemoval(absPath);
}

// help 명령어
void help_command() {
    printf("Usage:\n");
    printf("  > show\n");
    printf("    <none> : show monitoring daemon process info\n\n");

    printf("  > add <DIR_PATH> [OPTION]...\n");
    printf("    <none> : add daemon process monitoring the <DIR_PATH> directory\n");
    printf("    -d <OUTPUT_PATH> : Specify the output directory <OUTPUT_PATH> where <DIR_PATH> will be arranged\n");
    printf("    -i <TIME_INTERVAL> : Set the time interval for the daemon process to monitor in seconds.\n");
    printf("    -l <MAX_LOG_LINES> : Set the maximum number of log lines the daemon process will record.\n");
    printf("    -x <EXCLUDE_PATH1, EXCLUDE_PATH2, ...> : Exclude all subfiles in the specified directories.\n");
    printf("    -e <EXTENSION1, EXTENSION2, ...> : Specify the file extensions to be organized.\n");
    printf("    -m <M> : Specify the value for the <M> option.\n\n");

    printf("  > modify <DIR_PATH> [OPTION]...\n");
    printf("    <none> : modify daemon process config monitoring the <DIR_PATH> directory\n");
    printf("    -d <OUTPUT_PATH> : Specify the output directory <OUTPUT_PATH> where <DIR_PATH> will be arranged\n");
    printf("    -i <TIME_INTERVAL> : Set the time interval for the daemon process to monitor in seconds.\n");
    printf("    -l <MAX_LOG_LINES> : Set the maximum number of log lines the daemon process will record.\n");
    printf("    -x <EXCLUDE_PATH1, EXCLUDE_PATH2, ...> : Exclude all subfiles in the specified directories.\n");
    printf("    -e <EXTENSION1, EXTENSION2, ...> : Specify the file extensions to be organized.\n");
    printf("    -m <M> : Specify the value for the <M> option.\n\n");

    printf("  > remove <DIR_PATH>\n");
    printf("    (none) : remove daemon process monitoring the <DIR_PATH> directory\n\n");

    printf("  > help\n");
    printf("  > exit\n");
}

/*
 * main 함수:
 * 초기화 작업(데몬 리스트 복원, 홈 디렉토리 초기화)을 수행하고,
 * 사용자 입력을 받아 각 명령어(add, show, modify, remove, help, exit)를 처리하는 루프를 실행.
 */
int main() {
    // 데몬 목록 복원 및 홈 디렉토리 초기화
    restoreDaemonListFromFile();
    initHomeDir();

    char input[MY_MAX_INPUT];

    while (1) {
        // 프롬프트 출력 및 사용자 입력 대기
        printf("20211519> ");
        fflush(stdout);

        if (!fgets(input, sizeof(input), stdin))
            continue;
        input[strcspn(input, "\n")] = '\0';
        if (strlen(input) == 0)
            continue;
        // 입력 문자열을 공백으로 구분하여 첫 토큰(명령어) 추출
        char* command = strtok(input, " ");
        if (!command)
            continue;
        if (strcmp(command, "add") == 0) {
            char* arguments = strtok(NULL, "\n");
            add_command(arguments);
        }
        else if (strcmp(command, "show") == 0) {
            show_command();
        }
        else if (strcmp(command, "modify") == 0) {
            char* arguments = strtok(NULL, "\n");
            modify_command(arguments);
        }
        else if (strcmp(command, "remove") == 0) {
            char* arguments = strtok(NULL, "\n");  // remove 명령어 이후 인자들 처리
            remove_command(arguments);
        }
        else if (strcmp(command, "help") == 0) {
            help_command();
        }
        else if (strcmp(command, "exit") == 0) {
            break;
        }
        else {
            help_command();
        }
    }
    return 0;
}
