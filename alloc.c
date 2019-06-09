
#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include"spinlock.h"

// vma start的起始地址
#define VMA_ALLOC_FLOOR 0x4000

struct vmaTable{
    struct vma* storage;        // 系统预留的VMA仓库
    struct spinlock lock;       // 操作storage时的自旋锁
    int total_count;
    int left_count;
};

extern int mappages(pde_t*, void*, uint, uint, int);
pte_t * walkpgdir(pde_t*, const void*, int );

struct vmaTable vmaTable;


void initVMA(struct vma* vma){
    vma->start = -1;
    vma->length = vma->pageSize = 0;
    vma->prev = vma->next = 0;
}


// 使用首次适应算法，找到vm结构体中可以放下nbytes的Location
struct vma* findVMALocation(struct vm* vm, uint nbytes){
    struct vma* result = 0;
    struct vma* pVMA = vm->vmalist;
    while( pVMA && pVMA->next ){
        struct vma* pNext = pVMA->next;
        if( pNext->start - ( pVMA->start + pVMA->pageSize ) >= nbytes ){
            break;
        }
        pVMA = pVMA->next;
    }
    result = pVMA;
    return result;
}

// 从vmaTable中找到一个未使用的vma
struct vma* findFreeVMA(){
    int i;
    struct vma* result = 0;
    acquire(&vmaTable.lock);
    for( i = 0; i < vmaTable.total_count; i++ ){
        if( vmaTable.storage[i].start < 0 ){
            result = &vmaTable.storage[i];
            result->start = 0;
            break;
        }
    }
    release(&vmaTable.lock);
    return result;
}
// 回收一个VMA
void recycleVMA(struct vma* vma){
    initVMA(vma);
}

// 初始化vm结构体
void initProcVM(struct vm* vm){
    vm->vmalist = 0;
    vm->vma_count = 0;
}


// 初始化Mylloc文件
void initAlloc(){
    initlock(&vmaTable.lock, "vmaTable");
    
    // 为table分配初始的vma储备
    vmaTable.storage = (struct vma*)kalloc();
    // 对创建好的storage进行初始化
    int vmaSize = sizeof(struct vma);
    int vmaCountPerPage = PGSIZE / vmaSize;

    vmaTable.total_count = vmaCountPerPage;
    vmaTable.left_count = vmaTable.total_count;
    int i;
    for( i = 0; i < vmaTable.total_count; i++ ){
        initVMA( &vmaTable.storage[i] );
    }
}

// 拷贝VMA结构体及其内容
void copyVMA( struct vma* dest, struct vma* res ){
    dest->start = res->start;
    dest->length = res->length;
    dest->pageSize = res->pageSize;
}

// 拷贝VM结构体，并对VM中的内容进行映射
struct vm* copyVM( pde_t* pgdirDest, pde_t* pgdirSrc, struct vm* vm){
    struct vm* result = (struct vm*)kalloc();
    result->vmalist = 0;
    result->vma_count = 0;

    pte_t* pte;
    uint paSrc;
    uint paDest;
    uint flags;

    if( vm && vm->vmalist != 0 ){
        struct vma* pVMA = vm->vmalist;
        while( pVMA != 0 ){
            // 进行内存分配
            void* addr = alloc(pgdirDest, result, pVMA->length);
            // 复制内容
            if( (pte = walkpgdir( pgdirSrc, (void*)pVMA->start, 0 )) == 0){
                panic("copyVM: pte should exist");
            }
            if( !(*pte & PTE_P) ){
                panic("copyVM: page not present");
            }
            paSrc = PTE_ADDR(*pte);
            flags = PTE_FLAGS(*pte);

            pte = walkpgdir( pgdirDest, addr, flags );
            paDest = PTE_ADDR(*pte);

            memmove( (void*)P2V(paDest), (void*)P2V(paSrc), pVMA->length );
            pVMA = pVMA->next;
        }
    }
    return result;
}

void* alloc(pde_t* pgdir, struct vm* vm, uint nbytes){
    struct vma* vma = findFreeVMA();
    void* result = 0;
    if( vma != 0 ){
        // 提高到一页大小的倍数
        uint nbytes_pageup = PGROUNDUP(nbytes);
        // 需要的物理页帧数
        int pageNumNeeded = nbytes_pageup / PGSIZE;

        // 找到要放置的下一个vma
        struct vma* vmaLocation = findVMALocation(vm, nbytes_pageup);
        // 填写vma的信息，并将vma注册到结构体vm当中
        if( vmaLocation ){
            vma->start = vmaLocation->start + vmaLocation->pageSize;
            vma->next = vmaLocation->next;
            vmaLocation->next->prev = vma;
            vmaLocation->next = vma;
            vma->prev = vmaLocation;
        }else{
            vma->start = VMA_ALLOC_FLOOR;
            vma->next = vma->prev = 0;
            vm->vmalist = vma;
        }
        vma->length = nbytes;
        vma->pageSize = nbytes_pageup;

        int i;
        for( i = 0 ; i < pageNumNeeded; i++ ){
            char* temp = kalloc();
            memset(temp, 0, PGSIZE);
            // 映射
            mappages( pgdir, (void*)(vma->start + i * PGSIZE), PGSIZE, V2P(temp), PTE_W | PTE_U );
        }
        result = (void*)vma->start;
        vm->vma_count++;
    }
    return result;
}

// 释放指向的内存空间
// 往往是Alloc的返回结果
int free( pde_t* pgdir, struct vm* vm, void* addr){
    if( vm == 0 ){
        return -1;
    }
    // 遍历vm，找到起始地址和addr相同的vma
    struct vma* vmaLocation = 0;

    struct vma* pPrevVMA = 0;
    struct vma* pVMA = vm->vmalist;
    while( pVMA != 0 ){
        if( (void*)pVMA->start == addr ){
            vmaLocation = pVMA;
            break;
        }
        pPrevVMA = pVMA;
        pVMA = pVMA->next;
    }
    // 没有找到地址相同的则返回-1
    if( vmaLocation == 0 ){
        return -1;
    }
    
    // 解除页表映射
    uint pageSize = vmaLocation->pageSize;
    uint va = vmaLocation->start;
    uint endva = va + pageSize - 1;

    uint pa;
    pte_t* pte;
    for( ; va < endva ; va += PGSIZE ){
        pte = walkpgdir( pgdir, (char*)va, 0 );
        if( !pte ){
            va += (NPTENTRIES - 1) * PGSIZE;
        }else if( (*pte & PTE_P) != 0 ){
            pa = PTE_ADDR(*pte);
            kfree( (char*)P2V(pa) );
            *pte = 0;
        }
    }
    
    // @TODO: 修改vm结构体
    // 判断vmaLocation是否是header
    if( pPrevVMA == 0 ){
        vm->vmalist = vmaLocation->next;
        if( vm->vmalist != 0 )
            vm->vmalist->prev = 0;
    }else{
        pPrevVMA->next = vmaLocation->next;
        if( vmaLocation->next != 0 )
            vmaLocation->next->prev = pPrevVMA;
    }
    // 回收vma
    recycleVMA(vmaLocation);
   vm->vma_count--;
   return 1;
