
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <getopt.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#define PATH_MAX_LEN 4096
#define SUPERBLOCK_OFFSET 1024    // 슈퍼블록이 시작되는 바이트 오프셋
#define EXT2_NAME_LEN 255         // 디렉토리 엔트리 이름 최대 길이
#define EXT2_FT_DIR 2  // ext2_dir_entry에서 디렉토리 타입 값
// 전역 변수: 블록 크기, inode 크기, 그룹당 inode 수
uint32_t block_size;
uint32_t inode_size;
uint32_t inodes_per_group;

// ext2 inode 구조체 정의
struct ext2_inode {
    uint16_t i_mode;         // 파일 타입 및 권한
    uint16_t i_uid;          // 소유자 UID
    uint32_t i_size;         // 파일 크기 (바이트)
    uint32_t i_atime;        // 마지막 접근 시간
    uint32_t i_ctime;        // 생성 시간
    uint32_t i_mtime;        // 마지막 수정 시간
    uint32_t i_dtime;        // 삭제 시간
    uint16_t i_gid;          // 그룹 GID
    uint16_t i_links_count;  // 링크 수
    uint32_t i_blocks;       // 할당된 블록 수
    uint32_t i_flags;        // 플래그
    uint32_t osd1;           // OS 설정 필드
    uint32_t i_block[15];    // 직접/간접 블록 포인터
};




// ext2 슈퍼블록 구조체 정의
struct ext2_super_block {
    uint32_t s_inodes_count;     // 전체 inode 개수
    uint32_t s_blocks_count;     // 전체 블록 개수
    uint32_t s_r_blocks_count;   // 예약된 블록 개수
    uint32_t s_free_blocks_count;// 남은 블록 개수
    uint32_t s_free_inodes_count;// 남은 inode 개수
    uint32_t s_first_data_block; // 첫 데이터 블록 인덱스
    uint32_t s_log_block_size;   // 블록 크기 로그 (1024 << 값)
    uint32_t s_log_frag_size;    // fragment 크기 로그
    uint32_t s_blocks_per_group; // 그룹당 블록 수
    uint32_t s_frags_per_group;  // 그룹당 fragment 수
    uint32_t s_inodes_per_group; // 그룹당 inode 수
    uint32_t s_mtime;            // 마지막 체크 시간
    uint32_t s_wtime;            // 마지막 쓰기 시간
    uint16_t s_mnt_count;        // 마운트 횟수
    int16_t  s_max_mnt_count;    // 최대 마운트 횟수
    uint16_t s_magic;            // 마법번호 (0xEF53)
    uint16_t s_state;            // 파일 시스템 상태
    uint16_t s_errors;           // 오류 동작 방식
    uint16_t s_minor_rev_level;  // 마이너 리비전 레벨
    uint32_t s_lastcheck;        // 마지막 체크 시각
    uint32_t s_checkinterval;    // 체크 주기
    uint32_t s_creator_os;       // 생성 OS
    uint32_t s_rev_level;        // 리비전 레벨
    uint16_t s_def_resuid;       // UID 기본 권한
    uint16_t s_def_resgid;       // GID 기본 권한
    uint32_t s_first_ino;        // 첫 할당 가능한 inode 번호
    uint16_t s_inode_size;       // inode 구조 크기
};

// ext2 그룹 디스크립터 구조체 정의
struct ext2_group_desc {
    uint32_t bg_block_bitmap;    // 블록 비트맵 블록 번호
    uint32_t bg_inode_bitmap;    // inode 비트맵 블록 번호
    uint32_t bg_inode_table;     // inode 테이블 시작 블록 번호
    uint16_t bg_free_blocks_count;
    uint16_t bg_free_inodes_count;
    uint16_t bg_used_dirs_count;
    uint16_t bg_pad;
    uint32_t bg_reserved[3];
};


// ext2 디렉토리 엔트리 구조체 정의
struct ext2_dir_entry {
    uint32_t inode;   // 해당 파일/디렉토리의 inode 번호
    uint16_t rec_len; // 이 엔트리의 전체 길이
    uint8_t  name_len;// 이름 길이
    uint8_t  file_type;// 파일 타입 (1=file,2=dir)
    char     name[EXT2_NAME_LEN];
};

// 트리 구조용 노드 정의
typedef struct Node {
    char* name;              // 파일 또는 디렉토리 이름
    uint32_t inode_no;       // 해당 inode 번호
    uint8_t file_type;       // 파일 타입
    struct Node* first_child;// 첫 번째 자식 노드 포인터
    struct Node* next_sibling;// 다음 형제 노드 포인터
} Node;


// 전역 파일 디스크립터, 슈퍼블록, 그룹 디스크립터, 트리 루트
int img_fd;
struct ext2_super_block sb;
struct ext2_group_desc gd;
Node* root;

// 함수 프로토타입
void read_inode(int img_fd, uint32_t ino, struct ext2_inode* inode);
void read_superblock(int img_fd, struct ext2_super_block *sb);
void read_group_desc(int img_fd, uint32_t group, struct ext2_group_desc *gd, uint32_t block_size);

void insert_child_sorted(Node* parent, Node* child);
void build_tree(Node* parent);
void format_perm(uint16_t mode, char buf[11]);
void print_tree(Node* n, const char* prefix, int recursive, int show_size, int show_perm);
void count_tree(Node* n, int* dirs, int* files);

void command_tree(const char* path, int recursive, int show_size, int show_perm);
void command_print(const char* path, int max_lines);
void command_help(const char* cmd);
void command_help_all();
void command_help_tree();
void command_help_print();
void command_help_exit();
void command_help_help();

bool validate_path(const char *path);
Node* find_node(Node* current, const char* path);
Node* create_node(const char* name, uint32_t ino, uint8_t type);
void free_tree(Node* n);

int collect_data_blocks(int img_fd, const struct ext2_inode *ino, unsigned int block_size, uint32_t **out_blocks);

int main(int argc, char* argv[]) {
    // 인자 개수 검증
    if (argc != 2) {
        fprintf(stderr, "Usage Error : %s <EXT2_IMAGE>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // ext2 이미지 파일 오픈
    img_fd = open(argv[1], O_RDONLY);
    if (img_fd < 0) {
        perror("open");
        exit(EXIT_FAILURE);
    }

    // 슈퍼블록과 첫 번째 그룹 디스크립터 로드
    read_superblock(img_fd, &sb);

    read_group_desc(img_fd, 0, &gd, block_size);

    // 루트 노드 생성 (inode 2는 ROOT)
    root = create_node("/", 2, /*EXT2_FT_DIR=*/2);
    build_tree(root);  // 디렉토리 구조 트리 빌드

    // 명령 대기 루프
    char line[256];
    while (1) {
        printf("20211519> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        char* save = strdup(line);
        char* cmd = strtok(line, " \t\n");
        if (!cmd) {
            free(save);
            continue;
        }
        // tree 명령어
        if (strcmp(cmd, "tree") == 0) {
            int r = 0, s = 0, p = 0;
            char* path = NULL;
            char* tok = strtok(NULL, " \t\n");
            int invalid = 0;

            while (tok) {
                if (tok[0] == '-') {
                    // 옵션 문자열 한 글자씩 검사
                    for (int i = 1; tok[i]; i++) {
                        if (tok[i] == 'r') {
                            if (r) { invalid = 1; break; }  // 이미 -r 이 켜진 상태라면 error
                            r = 1;
                        }
                        else if (tok[i] == 's') {
                            if (s) { invalid = 1; break; }
                            s = 1;
                        }
                        else if (tok[i] == 'p') {
                            if (p) { invalid = 1; break; }
                            p = 1;
                        }
                        else {
                            invalid = 1;  // 정의되지 않은 옵션 문자
                            break;
                        }
                    }
                    if (invalid) break;
                }
                else {
                    if (!path) {
                        path = tok; // 첫 번째 비-옵션 토큰은 경로
                    }
                    else {
                        // 이미 경로가 정해졌는데 또 나왔다면 에러
                        invalid = 1;
                        break;
                    }
                }
                tok = strtok(NULL, " \t\n");
            }

            // 잘못된 옵션이 하나라도 있으면 Usage만 출력하고 루프 재시작
            if (invalid) {
                command_help_tree();
                free(save);
                continue;
            }

            if (!path) path = ".";
            if (!validate_path(path)) { 
	        free(save);
	        continue; 
	    }

            command_tree(path, r, s, p);
            free(save);
            continue;
        }
        //  print 분기
        else if (strcmp(cmd, "print") == 0) {
            int n = 0;
	    bool has_n = false;
	    bool zero_n = false;
            int invalid = 0, missing_arg = 0;
            char* path = NULL;
            char* tok = strtok(NULL, " \t\n");

	    while (tok) {
	        if (strcmp(tok, "-n") == 0) {
	            has_n = true;
	            tok = strtok(NULL, " \t\n");
	            if (!tok) {           // 숫자 없이 -n만 들어온 경우
	                missing_arg = 1;
	                break;
	            }
	            int raw_n = atoi(tok);
	            if (raw_n < 0) {      // 음수면 에러
	                fprintf(stderr, "print: invalid number of lines: %d\n", raw_n);
	                invalid = 1;
	                break;
	            }
	            if (raw_n == 0) {     // 0이면 출력 없이 바로 프롬프트
	                zero_n = true;
	                break;
	            }
	            n = raw_n;            // 정상 양수
	        }
	        else if (!path) {
	            path = tok;           // 첫 번째 non-option은 경로
	        }
	        else {
	            invalid = 1;          // 알 수 없는 토큰
	            break;
	        }
	        tok = strtok(NULL, " \t\n");
	    }

            if(invalid){
                command_help_print();
                free(save);
                continue;
            }
            if (missing_arg) {
                fprintf(stderr, "print: option requires an argument -- 'n'\n\n");
                free(save);
                continue;
            }
	    if(zero_n){
		free(save);
		continue;
	    }

            if (!path) {
                command_help_print();
                free(save);
                continue;
            }

            if (!validate_path(path)) { 
                free(save);
                continue; 
            }

            // 노드 찾기
            Node* tgt = find_node(root, path);
            if (!tgt) {
                command_help_print();
                free(save);
                continue;
            }
            if (tgt->file_type != 1) {
                fprintf(stderr, "Error: '%s' is not file\n\n", path);
                free(save);
                continue;
            }

            // 실제 출력
            command_print(path, has_n ? n : 0);
            free(save);
            continue;
        }

        // help 명령어
        else if (strcmp(cmd, "help") == 0) {
            char* arg = strtok(NULL, " \t\n");
            command_help(arg);
        }
        // exit
        else if (strcmp(cmd, "exit") == 0) {
            break;
        }
        else {
            command_help_all();
        }
    }

    // 메모리 해제 및 파일 닫기
    free_tree(root);
    close(img_fd);
    return 0;
}

// inode 로드: 그룹/인덱스 계산 후 해당 위치에서 읽기
void read_inode(int img_fd, uint32_t ino, struct ext2_inode *inode) {
    // 1) 그룹 번호, 그룹 내 인덱스
    uint32_t group = (ino - 1) / inodes_per_group;
    uint32_t index = (ino - 1) % inodes_per_group;

    // 2) 해당 그룹 디스크립터 읽기
    struct ext2_group_desc gd;
    read_group_desc(img_fd, group, &gd, block_size);

    // 3) inode 테이블 시작 오프셋
    off_t tbl_off = (off_t)gd.bg_inode_table * block_size;

    // 4) 테이블 내 offset
    off_t ino_off = tbl_off + (off_t)index * inode_size;

    // 5) 실제 inode 읽기
    if (pread(img_fd, inode, sizeof(*inode), ino_off) != sizeof(*inode)) {
        perror("pread inode, error");
        exit(EXIT_FAILURE);
    }

    
}

// 슈퍼블록 로드: offset 1024에서 읽어와 전역 블록 크기/정수 설정
void read_superblock(int img_fd, struct ext2_super_block *sb) {
    // 1) superblock 읽기 (offset 1024)
    if (pread(img_fd, sb, sizeof(*sb), SUPERBLOCK_OFFSET) != sizeof(*sb)) {
        perror("pread superblock, error");
        exit(EXIT_FAILURE);
    }


    // 2) block_size = 1024 << s_log_block_size
    block_size         = 1024 << sb->s_log_block_size;
    inode_size         = sb->s_inode_size;
    inodes_per_group   = sb->s_inodes_per_group;
}

// 그룹 디스크립터 로드: 특정 그룹 번호에 맞는 디스크립터를 읽음
void read_group_desc(int img_fd,
                     uint32_t group,
                     struct ext2_group_desc *gd,
                     uint32_t block_size)
{
    const size_t GD_SIZE = sizeof(*gd);  // 32

    // 1) 슈퍼블록이 속한 블록 번호
    uint32_t sb_block     = SUPERBLOCK_OFFSET / block_size;  
    // 2) 그룹 디스크립터 테이블 시작 블록 번호
    uint32_t gd_table_blk = sb_block + 1;                     
    // 3) 바이트 오프셋
    off_t    off         = (off_t)gd_table_blk * block_size
                          + (off_t)group * GD_SIZE;

    if (pread(img_fd, gd, GD_SIZE, off) != GD_SIZE) {
        perror("pread group_desc");
        exit(EXIT_FAILURE);
    }
}

// 노드 생성: 이름, inode 번호, 타입으로 초기화
Node* create_node(const char* name, uint32_t ino, uint8_t type) {
    Node* n = malloc(sizeof(Node));
    n->name = strdup(name);
    n->inode_no = ino;
    n->file_type = type;
    n->first_child = NULL;
    n->next_sibling = NULL;
    return n;
}


// 자식 노드를 '디렉토리 우선, 같은 타입 내에서는 이름 사전순'으로 삽입
void insert_child_sorted(Node* parent, Node* child) {
    Node **cur = &parent->first_child;
    while (*cur) {
        Node *cur_node = *cur;
        // 1) child가 디렉토리이고 cur_node가 파일이면, 지금(cur)이 삽입 위치
        if (child->file_type == EXT2_FT_DIR && cur_node->file_type != EXT2_FT_DIR) {
            break;
        }
        // 2) child가 파일이고 cur_node가 디렉토리이면, 디렉토리 뒤로 건너뜀
        if (child->file_type != EXT2_FT_DIR && cur_node->file_type == EXT2_FT_DIR) {
            cur = &cur_node->next_sibling;
            continue;
        }
        // 3) 동일 타입(둘 다 디렉토리이거나 둘 다 파일)일 때 이름 비교
        if (strcmp(cur_node->name, child->name) < 0) {
            cur = &cur_node->next_sibling;
        } else {
            break;
        }
    }
    // child를 cur 위치에 삽입
    child->next_sibling = *cur;
    *cur = child;
}


// 디렉토리 트리 구성: direct+indirect 블록 모두 순회 → 디렉토리 엔트리 읽기 → 자식 노드 생성/재귀
void build_tree(Node* parent) {
    struct ext2_inode ino;
    read_inode(img_fd, parent->inode_no, &ino);

    // 1) collect_data_blocks 로 모든 데이터 블록 번호(직접·간접) 얻어오기
    uint32_t *blocks = NULL;
    int nblocks = collect_data_blocks(img_fd, &ino, block_size, &blocks);
    if (nblocks < 0) {
        perror("collect_data_blocks 실패");
        return;
    }

    // 2) 블록별로 디렉토리 엔트리 파싱
    for (int bi = 0; bi < nblocks; bi++) {
        off_t blk_off = (off_t)blocks[bi] * block_size;
        off_t cur = 0;

        while (cur < block_size) {
            struct ext2_dir_entry e;
            // entry header
            pread(img_fd, &e,
                  offsetof(struct ext2_dir_entry, name),
                  blk_off + cur);
            if (e.inode == 0) break;

            // name 읽기
            char name[EXT2_NAME_LEN + 1] = { 0 };
            pread(img_fd, name, e.name_len,
                  blk_off + cur + offsetof(struct ext2_dir_entry, name));
            name[e.name_len] = '\0';

            // '.', '..', 'lost+found' 제외
            if (e.inode
                && strcmp(name, ".")
                && strcmp(name, "..")
                && strcmp(name, "lost+found"))
            {
                Node* child = create_node(name, e.inode, e.file_type);
                insert_child_sorted(parent, child);

                // 디렉토리면 재귀 호출
                if (e.file_type == EXT2_FT_DIR)
                    build_tree(child);
            }

            cur += e.rec_len;  // 다음 엔트리
        }
    }

    free(blocks);
}




// 트리 출력: 재귀/크기/권한 옵션에 따라 분기
void print_tree(Node* n, const char* prefix,
    int recursive, int show_size, int show_perm)
{   
    // 현재 노드(n)의 첫 번째 자식부터 순회
    Node* c = n->first_child;
    while (c) {
        // '.', '..', 'lost+found' 디렉토리는 건너뛴다
        if (strcmp(c->name, ".") == 0 ||
            strcmp(c->name, "..") == 0 ||
            strcmp(c->name, "lost+found") == 0)
        {
            c = c->next_sibling;
            continue;
        }
        // 마지막 형제인지 판별 (branch 그릴 때 └ 혹은 ├ 선택)
        bool is_last = (c->next_sibling == NULL);
        const char* branch = is_last ? "└" : "├";

        // 옵션이 모두 켜진 경우: 권한 + 크기
        if (show_perm && show_size) {
            // 권한과 크기를 한 줄로: [perm size]
            struct ext2_inode ino;
            read_inode(img_fd, c->inode_no, &ino);

            char perm[11];
            format_perm(ino.i_mode, perm);
            printf("%s%s [%s %u] %s",
                   prefix, branch, perm, ino.i_size, c->name);
        }

        // 크기만 보여주기
        else if (show_size) {
            struct ext2_inode ino;
            read_inode(img_fd, c->inode_no, &ino);
            printf("%s%s [%u] %s", prefix, branch, ino.i_size, c->name);
        }
        else if (show_perm) {
            // 권한만: [perm]
            struct ext2_inode ino;
            read_inode(img_fd, c->inode_no, &ino);

            char perm[11];
            format_perm(ino.i_mode, perm);
            printf("%s%s [%s] %s",
                   prefix, branch, perm, c->name);
        }
        // 옵션 없는 경우: 이름만
	    else{
            printf("%s%s %s", prefix, branch, c->name);
	    }
	    printf("\n");

        // 디렉토리이고 재귀 옵션이 켜진 경우, 하위로 내려가서 출력
        if (recursive && c->file_type == 2) {
            char np[256];
            // 다음 레벨에서 prefix로 사용할 문자열 생성
            // 마지막 형제면 공백, 아니면 “│ ”
            snprintf(np, sizeof(np), "%s%s",
                prefix, is_last ? " " : "│ ");
            print_tree(c, np, recursive, show_size, show_perm);
        }
        // 다음 형제 노드로 이동
        c = c->next_sibling;
    }
}

// 트리 내 디렉토리/파일 개수 세기
void count_tree(Node* n, int* dirs, int* files) {
    // 첫 번째 자식 노드부터 탐색
    Node* c = n->first_child;
    while (c) {
        // '.' '..' 'lost+found' 은 결과에 포함하지 않음
        if (strcmp(c->name, ".") == 0 ||
            strcmp(c->name, "..") == 0 ||
            strcmp(c->name, "lost+found") == 0)
        {
            c = c->next_sibling;
            continue;
        }
        // 디렉토리인 경우
        if (c->file_type == 2) {
            (*dirs)++;    // 디렉토리 카운트 증가
            count_tree(c, dirs, files);   // 재귀적으로 하위 트리도 세기
        }
        // 파일인 경우
        else {
            (*files)++;  // 파일 카운트 증가
        }
        // 다음 형제 노드로 이동
        c = c->next_sibling;
    }
}

// 파일/디렉토리 권한 비트(mode)를 문자열("drwxr-xr-x")로 변환
void format_perm(uint16_t mode, char buf[11]) {
    // 첫 문자는 디렉토리 여부 표시: 디렉토리이면 'd', 아니면 '-'
    buf[0] = (mode & S_IFDIR) ? 'd' : '-';
    // 사용자(owner), 그룹(group), 기타(other)에 대한 읽기(r), 쓰기(w), 실행(x) 권한 문자
    const char* rwx = "rwx";

    // 9비트 권한(rwx rwx rwx)을 하나씩 검사하여 buf[1]~buf[9]에 채워넣음
    // 가장 상위 비트부터 차례대로 검사 (비트 8이 owner 읽기, 비트 0이 other 실행)
    for (int i = 0; i < 9; i++) {
        // (1 << (8 - i)) 위치에 해당하는 비트가 켜져 있으면
        // rwx 문자열에서 i%3 인덱스 문자('r','w','x')를, 아니면 '-
        buf[i + 1] = (mode & (1 << (8 - i))) ? rwx[i % 3] : '-';
    }
    // 문자열 끝에 NULL 문자 추가
    buf[10] = '\0';
}

// tree 명령어
void command_tree(const char* path, int recursive,
              int show_size, int show_perm)
{
    // 경로에 해당하는 노드 찾기
    Node* tgt = find_node(root, path);
    if (!tgt) {
        // 존재하지 않는 경로면 도움말 출력
        command_help_tree();
        return;
    }
    //  디렉토리가 아니면 에러
    if (tgt->file_type != 2) {
        fprintf(stderr, "Error: '%s' is not directory\n", path);
        return;
    }

    //  디렉토리 inode 정보 읽기
    struct ext2_inode ino;
    read_inode(img_fd, tgt->inode_no, &ino);

    // 권한+크기, 권한, 크기, 아무 옵션 없을 때 로 분기
    if (show_perm && show_size) {
        char perm[11];
        format_perm(ino.i_mode, perm);
        printf("[%s %u] %s\n",
               perm, ino.i_size,
               strcmp(path, "/") == 0 ? "." : path);
    }
    else if (show_perm) {
        char perm[11];
        format_perm(ino.i_mode, perm);
        printf("[%s] %s\n",
               perm,
               strcmp(path, "/") == 0 ? "." : path);
    }
    else if (show_size) {
        printf("[%u] %s\n",
               ino.i_size,
               strcmp(path, "/") == 0 ? "." : path);
    }
    else {
        // 옵션 없으면 기존 방식
        printf("%s\n",
               strcmp(path, "/") == 0 ? "." : path);
    }
    //  실제 트리 구조 출력 (자식 노드들)
    print_tree(tgt, "", recursive, show_size, show_perm);

    //  요약용 카운트
    int dirs = 0, files = 0;
    Node* c = tgt->first_child;
    while (c) {
        // 스킵할 이름들
        if (strcmp(c->name, ".") == 0 ||
            strcmp(c->name, "..") == 0 ||
            strcmp(c->name, "lost+found") == 0)
        {
            c = c->next_sibling;
            continue;
        }
        if (c->file_type == 2) {
            dirs++;
            // -r 옵션일 때만 하위도 재귀 카운트
            if (recursive) {
                count_tree(c, &dirs, &files);
            }
        }
        else {
            files++;
        }
        c = c->next_sibling;
    }
    // 항상 대상 디렉터리 자신도 하나의 directory 로 카운트
    dirs++;

    printf("\n%d directories, %d files\n\n", dirs, files);
}

// print 명령어
void command_print(const char* path, int max_lines) {
    // 대상 노드 찾기 및 inode 읽기
    Node* tgt = find_node(root, path);
    struct ext2_inode ino;
    read_inode(img_fd, tgt->inode_no, &ino);

    // --- 1) 모든 데이터 블록 번호 모으기 ---
    uint32_t *blocks = NULL;
    int nblocks = collect_data_blocks(img_fd, &ino, block_size, &blocks);

    // 출력 제한(max_lines)이 있을 경우, 실제 출력 전에
    //  파일에 줄이 더 있는지(has_more) 미리 검사
    // --- 2) has_more 검사 (줄 제한이 있을 때만) ---
    bool has_more = false;
    if (max_lines > 0) {
        char *buf = malloc(block_size);
        int counted = 0;
        for (int bi = 0; bi < nblocks && !has_more; bi++) {
            off_t off = (off_t)blocks[bi] * block_size;
            ssize_t got = pread(img_fd, buf, block_size, off);
            if (got <= 0) break;
            for (char *p = buf; p < buf + got; p++) {
                if (*p == '\n' && ++counted > max_lines) {
                    has_more = true;
                    break;
                }
            }
        }
        free(buf);
    }


    // --- 3) 실제 출력 ---
    char *tmp = malloc(block_size);
    char *line_buf = NULL;
    size_t line_cap = 0, line_len = 0;
    int printed = 0;

    for (int bi = 0;
         bi < nblocks && (max_lines == 0 || printed < max_lines);
         bi++)
    {
        off_t off = (off_t)blocks[bi] * block_size;
        ssize_t got = pread(img_fd, tmp, block_size, off);
        if (got <= 0) break;

        size_t pos = 0;
        while (pos < (size_t)got && (max_lines == 0 || printed < max_lines)) {
            // 다음 개행까지 또는 블록 끝까지 chunk 길이 계산
            char *nl = memchr(tmp + pos, '\n', got - pos);
            size_t chunk_len = nl
                               ? (size_t)(nl - (tmp + pos) + 1)
                               : (size_t)(got - pos);

            // line_buf 확장
            if (line_len + chunk_len + 1 > line_cap) {
                line_cap = (line_len + chunk_len + 1) * 2;
                line_buf = realloc(line_buf, line_cap);
            }
            // 조각 누적
            memcpy(line_buf + line_len, tmp + pos, chunk_len);
            line_len += chunk_len;
            line_buf[line_len] = '\0';

            pos += chunk_len;

            if (nl) {
                // 완성된 한 줄 출력
                fwrite(line_buf, 1, line_len, stdout);
                printed++;
                line_len = 0;  // 다음 줄을 위해 버퍼 리셋
            }
        }
    }
    free(tmp);
    free(line_buf);
    free(blocks);


    // --- 4) 더 볼 내용이 있으면 빈 줄 추가 ---
    if (max_lines > 0 && printed == max_lines && has_more) {
        putchar('\n');
    }


}

// help 명령어
void command_help(const char* cmd) {
    if (cmd == NULL) {
        // 기본 help
        command_help_all();
    }
    // tree 명령어 help
    else if (strcmp(cmd, "tree") == 0) {
        command_help_tree();
    }
    // print 명령어 help
    else if (strcmp(cmd, "print") == 0) {
        command_help_print();
    }

    // help 명령어 help
    else if (strcmp(cmd, "help") == 0) {
        command_help_help();
    }
    // exit 명령어 help
    else if (strcmp(cmd, "exit") == 0) {
        command_help_exit();
    }
    else {
        // 올바르지 않은 명령어
        fprintf(stderr, "invalid command -- '%s'\n", cmd);
        command_help_all();
    }
}

// 기본 help 명령어
void command_help_all() {
    printf("Usage :\n");
    printf("  > tree <PATH> [OPTION]... : display the directory structure if <PATH> is a directory\n");
    printf("    -r : display the directory structure recursively if <PATH> is a directory\n");
    printf("    -s : display the directory structure if <PATH> is a directory, including the size of each file\n");
    printf("    -p : display the directory structure if <PATH> is a directory, including the permissions of each directory and file\n");
    printf("  > print <PATH> [OPTION]... : print the contents on the standard output if <PATH> is a file\n");
    printf("    -n <line_number> : print only the first <line_number> lines of its contents on the standard output if <PATH> is file\n");
    printf("  > help [COMMAND] : show commands for program\n");
    printf("  > exit : exit program\n");
}

// tree 명령어 help
void command_help_tree() {
    printf("Usage :\n");
    printf("  > tree <PATH> [OPTION]... : display the directory structure if <PATH> is a directory\n");
    printf("    -r : display the directory structure recursively if <PATH> is a directory\n");
    printf("    -s : display the directory structure if <PATH> is a directory, including the size of each file\n");
    printf("    -p : display the directory structure if <PATH> is a directory, including the permissions of each directory and file\n");
}
// print 명령어 help
void command_help_print() {
    printf("Usage :\n");
    printf("  > print <PATH> [OPTION]... : print the contents on the standard output if <PATH> is a file\n");
    printf("    -n <line_number> : print only the first <line_number> lines of its contents on the standard output if <PATH> is file\n");
}
// exit 명령어 help
void command_help_exit() {
    printf("Usage :\n");
    printf("  > exit : exit program\n");
}
// help 명령어 help
void command_help_help() {
    printf("Usage :\n");
    printf("  > help [COMMAND] : show commands for program\n");
}


// 전체 경로 길이와 각 구성 요소 길이 검사
bool validate_path(const char *path) {
    size_t len = strlen(path);
    if (len > PATH_MAX_LEN) {
        fprintf(stderr, "Error: path length %zu exceeds maximum %d bytes\n",
                len, PATH_MAX_LEN);
        return false;
    }
    // 각 디렉터리/파일 이름 부분 검사
    // strdupa나 strdup을 쓰지 않고 안전하게 복사
    char buf[PATH_MAX_LEN + 1];
    strncpy(buf, path, PATH_MAX_LEN);
    buf[PATH_MAX_LEN] = '\0';

    char *p = buf;
    char *tok;
    while ((tok = strsep(&p, "/")) != NULL) {
        if (strlen(tok) > EXT2_NAME_LEN) {
            fprintf(stderr,
                    "Error: component '%s' length %zu exceeds maximum %d bytes\n",
                    tok, strlen(tok), EXT2_NAME_LEN);
            return false;
        }
    }
    return true;
}

// 경로 문자열(path)에 해당하는 노드를 트리에서 찾아 반환
// current: 상대 경로 탐색 시 기준이 될 노드 (대부분 root)
// path   : 절대("/") 또는 상대(".") 경로, 또는 "dir/sub/file" 등
Node* find_node(Node* current, const char* path) {
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0)
        return root;   // "/" 또는 "." 는 언제나 루트 디렉토리

    // 절대 경로라면 root부터, 상대 경로면 current 노드부터 탐색 시작
    Node* cur = (path[0] == '/') ? root : current;
    char* buf = strdup(path);
    char* tok = strtok(buf, "/");
    // 토큰(디렉토리/파일 이름)마다 하위 노드로 이동
    while (tok && cur) {
        Node* child = cur->first_child;
        Node* next = NULL;
        // 현재 노드(cur)의 자식들 중에서 이름이 일치하는 노드 찾기
        while (child) {
            if (strcmp(child->name, tok) == 0) {
                next = child;
                break;
            }
            child = child->next_sibling;
        }
        // 찾은 자식으로 현재 위치 이동
        cur = next;
        tok = strtok(NULL, "/");
    }
    // 복제했던 메모리 해제
    free(buf);
    return cur;
}


// 트리 구조의 모든 노드를 재귀적으로 해제하는 함수
void free_tree(Node* n) {
    //  자식 노드부터 순회하며 재귀적으로 해제
    Node* c = n->first_child;
    while (c) {
        Node* next = c->next_sibling;  // 다음 형제 노드를 미리 저장
        free_tree(c);   // 자식 서브트리 해제
        c = next;       // 다음 형제로 이동
    }
    //  현재 노드의 리소스 해제
    free(n->name);  // strdup으로 할당된 이름 문자열 메모리 해제
    free(n);   // 노드 구조체 메모리 해제
}

int collect_data_blocks(int img_fd,
                               const struct ext2_inode *ino,
                               unsigned int block_size,
                               uint32_t **out_blocks)
{
    unsigned int ptrs_per_block = block_size / sizeof(uint32_t);
    // direct(12) + single + double*ptrs_per + triple*ptrs_per*ptrs_per
    int cap = 12 + ptrs_per_block + ptrs_per_block*ptrs_per_block + ptrs_per_block*ptrs_per_block*ptrs_per_block;
    uint32_t *blocks = malloc(sizeof(uint32_t) * cap);
    int cnt = 0;

    // 1) direct blocks
    for (int i = 0; i < 12; i++)
        if (ino->i_block[i])
            blocks[cnt++] = ino->i_block[i];

    // 2) single indirect
    if (ino->i_block[12]) {
        uint32_t *ptrs = malloc(block_size);
        pread(img_fd, ptrs, block_size, (off_t)ino->i_block[12] * block_size);
        for (unsigned i = 0; i < ptrs_per_block; i++)
            if (ptrs[i])
                blocks[cnt++] = ptrs[i];
        free(ptrs);
    }

    // 3) double indirect
    if (ino->i_block[13]) {
        uint32_t *ind = malloc(block_size);
        pread(img_fd, ind, block_size, (off_t)ino->i_block[13] * block_size);
        for (unsigned i = 0; i < ptrs_per_block; i++) {
            if (!ind[i]) continue;
            uint32_t *ptrs = malloc(block_size);
            pread(img_fd, ptrs, block_size, (off_t)ind[i] * block_size);
            for (unsigned j = 0; j < ptrs_per_block; j++)
                if (ptrs[j])
                    blocks[cnt++] = ptrs[j];
            free(ptrs);
        }
        free(ind);
    }

    // 4) triple indirect
    if (ino->i_block[14]) {
        uint32_t *dbl = malloc(block_size);
        pread(img_fd, dbl, block_size, (off_t)ino->i_block[14] * block_size);
        for (unsigned i = 0; i < ptrs_per_block; i++) {
            if (!dbl[i]) continue;
            uint32_t *ind = malloc(block_size);
            pread(img_fd, ind, block_size, (off_t)dbl[i] * block_size);
            for (unsigned j = 0; j < ptrs_per_block; j++) {
                if (!ind[j]) continue;
                uint32_t *ptrs = malloc(block_size);
                pread(img_fd, ptrs, block_size, (off_t)ind[j] * block_size);
                for (unsigned k = 0; k < ptrs_per_block; k++)
                    if (ptrs[k])
                        blocks[cnt++] = ptrs[k];
                free(ptrs);
            }
            free(ind);
        }
        free(dbl);
    }

    *out_blocks = blocks;
    return cnt;
}
