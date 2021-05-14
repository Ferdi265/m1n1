/* SPDX-License-Identifier: MIT */

#include "hv.h"
#include "assert.h"
#include "cpu_regs.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

//#define dprintf printf
#define dprintf(...)                                                                               \
    do {                                                                                           \
    } while (0)

#define PAGE_SIZE       0x4000
#define CACHE_LINE_SIZE 64

#define PTE_ACCESS            BIT(10)
#define PTE_SH_NS             (0b11L << 8)
#define PTE_S2AP_RW           (0b11L << 6)
#define PTE_MEMATTR_UNCHANGED (0b1111L << 2)

#define PTE_ATTRIBUTES (PTE_ACCESS | PTE_SH_NS | PTE_S2AP_RW | PTE_MEMATTR_UNCHANGED)

#define PTE_LOWER_ATTRIBUTES GENMASK(13, 2)

#define PTE_VALID BIT(0)
#define PTE_TYPE  BIT(1)
#define PTE_BLOCK 0
#define PTE_TABLE 1
#define PTE_PAGE  1

#define VADDR_L4_INDEX_BITS 12
#define VADDR_L3_INDEX_BITS 11
#define VADDR_L2_INDEX_BITS 11

#define VADDR_L4_OFFSET_BITS 2
#define VADDR_L3_OFFSET_BITS 14
#define VADDR_L2_OFFSET_BITS 25

#define VADDR_L2_ALIGN_MASK GENMASK(VADDR_L2_OFFSET_BITS - 1, VADDR_L3_OFFSET_BITS)
#define VADDR_L3_ALIGN_MASK GENMASK(VADDR_L3_OFFSET_BITS - 1, VADDR_L4_OFFSET_BITS)
#define PTE_TARGET_MASK     GENMASK(49, VADDR_L3_OFFSET_BITS)
#define PTE_TARGET_MASK_L4  GENMASK(49, VADDR_L4_OFFSET_BITS)

#define ENTRIES_PER_L2_TABLE BIT(VADDR_L2_INDEX_BITS)
#define ENTRIES_PER_L3_TABLE BIT(VADDR_L3_INDEX_BITS)
#define ENTRIES_PER_L4_TABLE BIT(VADDR_L4_INDEX_BITS)

#define SPTE_TYPE BIT(50)
#define SPTE_MAP  0
#define SPTE_HOOK 1

#define IS_HW(pte) ((pte) && pte & PTE_VALID)
#define IS_SW(pte) ((pte) && !(pte & PTE_VALID))

#define L2_IS_TABLE(pte)     ((pte) && FIELD_GET(PTE_TYPE, pte) == PTE_TABLE)
#define L2_IS_NOT_TABLE(pte) ((pte) && !L2_IS_TABLE(pte))
#define L2_IS_HW_BLOCK(pte)  (IS_HW(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_BLOCK)
#define L2_IS_SW_BLOCK(pte)                                                                        \
    (IS_SW(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_BLOCK && FIELD_GET(SPTE_TYPE, pte) == SPTE_MAP)
#define L3_IS_TABLE(pte)     (IS_SW(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_TABLE)
#define L3_IS_NOT_TABLE(pte) ((pte) && !L3_IS_TABLE(pte))
#define L3_IS_HW_BLOCK(pte)  (IS_HW(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_PAGE)
#define L3_IS_SW_BLOCK(pte)                                                                        \
    (IS_SW(pte) && FIELD_GET(PTE_TYPE, pte) == PTE_BLOCK && FIELD_GET(SPTE_TYPE, pte) == SPTE_MAP)

/*
 * We use 16KB page tables for stage 2 translation, and a 64GB (36-bit) guest
 * PA size, which results in the following virtual address space:
 *
 * [L2 index]  [L3 index] [page offset]
 *  11 bits     11 bits    14 bits
 *
 * 32MB L2 mappings look like this:
 * [L2 index]  [page offset]
 *  11 bits     25 bits
 *
 * We implement sub-page granularity mappings for software MMIO hooks, which behave
 * as an additional page table level used only by software. This works like this:
 *
 * [L2 index]  [L3 index] [L4 index]  [Word offset]
 *  11 bits     11 bits    12 bits     2 bits
 *
 * Thus, L4 sub-page tables are twice the size.
 *
 * We use invalid mappings (PTE_VALID == 0) to represent mmiotrace descriptors, but
 * otherwise the page table format is the same. The PTE_TYPE bit is weird, as 0 means
 * block but 1 means both table (at L<3) and page (at L3). For mmiotrace, this is
 * pushed to L4.
 */

static u64 hv_L2[ENTRIES_PER_L2_TABLE] ALIGNED(PAGE_SIZE);

void hv_pt_init(void)
{
    memset(hv_L2, 0, sizeof(hv_L2));

    msr(VTCR_EL2, FIELD_PREP(VTCR_PS, 1) |        // 64GB PA size
                      FIELD_PREP(VTCR_TG0, 2) |   // 16KB page size
                      FIELD_PREP(VTCR_SH0, 3) |   // PTWs Inner Sharable
                      FIELD_PREP(VTCR_ORGN0, 1) | // PTWs Cacheable
                      FIELD_PREP(VTCR_IRGN0, 1) | // PTWs Cacheable
                      FIELD_PREP(VTCR_SL0, 1) |   // Start at level 2
                      FIELD_PREP(VTCR_T0SZ, 28)); // 64GB translation region

    msr(VTTBR_EL2, hv_L2);
}

static void hv_pt_free_l3(u64 *l3)
{
    if (!l3)
        return;

    for (u64 idx = 0; idx < ENTRIES_PER_L3_TABLE; idx++)
        if (IS_SW(l3[idx]) && FIELD_GET(PTE_TYPE, l3[idx]) == PTE_TABLE)
            free((void *)(l3[idx] & PTE_TARGET_MASK));
    free(l3);
}

static void hv_pt_map_l2(u64 from, u64 to, u64 size, u64 incr)
{
    assert((from & MASK(VADDR_L2_OFFSET_BITS)) == 0);
    assert(IS_SW(to) || (to & PTE_TARGET_MASK & MASK(VADDR_L2_OFFSET_BITS)) == 0);
    assert((size & MASK(VADDR_L2_OFFSET_BITS)) == 0);

    to |= FIELD_PREP(PTE_TYPE, PTE_BLOCK);

    for (; size; size -= BIT(VADDR_L2_OFFSET_BITS)) {
        u64 idx = from >> VADDR_L2_OFFSET_BITS;

        if (L2_IS_TABLE(hv_L2[idx]))
            hv_pt_free_l3((u64 *)(hv_L2[idx] & PTE_TARGET_MASK));

        hv_L2[idx] = to;
        from += BIT(VADDR_L2_OFFSET_BITS);
        to += incr * BIT(VADDR_L2_OFFSET_BITS);
    }
}

static u64 *hv_pt_get_l3(u64 from)
{
    u64 l2idx = from >> VADDR_L2_OFFSET_BITS;
    u64 l2d = hv_L2[l2idx];

    if (L2_IS_TABLE(l2d))
        return (u64 *)(l2d & PTE_TARGET_MASK);

    u64 *l3 = (u64 *)memalign(PAGE_SIZE, ENTRIES_PER_L3_TABLE * sizeof(u64));
    if (l2d) {
        u64 incr = 0;
        u64 l3d = l2d;
        if (IS_HW(l2d)) {
            l3d &= ~PTE_TYPE;
            l3d |= FIELD_PREP(PTE_TYPE, PTE_PAGE);
            incr = BIT(VADDR_L3_OFFSET_BITS);
        }
        for (u64 idx = 0; idx < ENTRIES_PER_L3_TABLE; idx++, l3d += incr)
            l3[idx] = l3d;
    } else {
        memset64(l3, 0, ENTRIES_PER_L3_TABLE * sizeof(u64));
    }

    l2d = ((u64)l3) | FIELD_PREP(PTE_TYPE, PTE_TABLE) | PTE_VALID;
    hv_L2[l2idx] = l2d;
    return l3;
}

static void hv_pt_map_l3(u64 from, u64 to, u64 size, u64 incr)
{
    assert((from & MASK(VADDR_L3_OFFSET_BITS)) == 0);
    assert((IS_SW(to) || to & PTE_TARGET_MASK & MASK(VADDR_L3_OFFSET_BITS)) == 0);
    assert((size & MASK(VADDR_L3_OFFSET_BITS)) == 0);

    if (IS_HW(to))
        to |= FIELD_PREP(PTE_TYPE, PTE_PAGE);
    else
        to |= FIELD_PREP(PTE_TYPE, PTE_BLOCK);

    for (; size; size -= BIT(VADDR_L3_OFFSET_BITS)) {
        u64 idx = (from >> VADDR_L3_OFFSET_BITS) & MASK(VADDR_L3_INDEX_BITS);
        u64 *l3 = hv_pt_get_l3(from);

        if (L3_IS_TABLE(l3[idx]))
            free((void *)(l3[idx] & PTE_TARGET_MASK));

        l3[idx] = to;
        from += BIT(VADDR_L3_OFFSET_BITS);
        to += incr * BIT(VADDR_L3_OFFSET_BITS);
    }
}

static u64 *hv_pt_get_l4(u64 from)
{
    u64 *l3 = hv_pt_get_l3(from);
    u64 l3idx = (from >> VADDR_L3_OFFSET_BITS) & MASK(VADDR_L3_INDEX_BITS);
    u64 l3d = l3[l3idx];

    if (L3_IS_TABLE(l3d)) {
        return (u64 *)(l3d & PTE_TARGET_MASK);
    }

    if (IS_HW(l3d)) {
        assert(FIELD_GET(PTE_TYPE, l3d) == PTE_PAGE);
        l3d &= PTE_TARGET_MASK;
        l3d |= FIELD_PREP(PTE_TYPE, PTE_BLOCK) | FIELD_PREP(SPTE_TYPE, SPTE_MAP);
    }

    u64 *l4 = (u64 *)memalign(PAGE_SIZE, ENTRIES_PER_L4_TABLE * sizeof(u64));
    if (l3d) {
        u64 incr = 0;
        u64 l4d = l3d;
        l4d &= ~PTE_TYPE;
        l4d |= FIELD_PREP(PTE_TYPE, PTE_PAGE);
        if (FIELD_GET(SPTE_TYPE, l4d) == SPTE_MAP)
            incr = BIT(VADDR_L4_OFFSET_BITS);
        for (u64 idx = 0; idx < ENTRIES_PER_L4_TABLE; idx++, l4d += incr)
            l4[idx] = l4d;
    } else {
        memset64(l4, 0, ENTRIES_PER_L4_TABLE * sizeof(u64));
    }

    l3d = ((u64)l4) | FIELD_PREP(PTE_TYPE, PTE_TABLE);
    l3[l3idx] = l3d;
    return l4;
}

static void hv_pt_map_l4(u64 from, u64 to, u64 size, u64 incr)
{
    assert((from & MASK(VADDR_L4_OFFSET_BITS)) == 0);
    assert((size & MASK(VADDR_L4_OFFSET_BITS)) == 0);

    assert(!IS_HW(to));

    if (IS_SW(to))
        to |= FIELD_PREP(PTE_TYPE, PTE_PAGE);

    for (; size; size -= BIT(VADDR_L4_OFFSET_BITS)) {
        u64 idx = (from >> VADDR_L4_OFFSET_BITS) & MASK(VADDR_L4_INDEX_BITS);
        u64 *l4 = hv_pt_get_l4(from);

        l4[idx] = to;
        from += BIT(VADDR_L4_OFFSET_BITS);
        to += incr * BIT(VADDR_L4_OFFSET_BITS);
    }
}

int hv_map(u64 from, u64 to, u64 size, u64 incr)
{
    u64 chunk;
    bool hw = IS_HW(to);

    if (from & MASK(VADDR_L4_OFFSET_BITS) || size & MASK(VADDR_L4_OFFSET_BITS))
        return -1;

    if (hw && (from & MASK(VADDR_L3_OFFSET_BITS) || size & MASK(VADDR_L3_OFFSET_BITS))) {
        printf("HV: cannot use L4 pages with HW mappings (0x%lx -> 0x%lx)\n", from, to);
        return -1;
    }

    // L4 mappings to boundary
    chunk = min(size, ALIGN_UP(from, MASK(VADDR_L3_OFFSET_BITS)) - from);
    if (chunk) {
        assert(!hw);
        hv_pt_map_l4(from, to, chunk, incr);
        from += chunk;
        to += incr * chunk;
        size -= chunk;
    }

    // L3 mappings to boundary
    chunk = ALIGN_DOWN(min(size, ALIGN_UP(from, MASK(VADDR_L2_OFFSET_BITS)) - from),
                       MASK(VADDR_L3_OFFSET_BITS));
    if (chunk) {
        hv_pt_map_l3(from, to, chunk, incr);
        from += chunk;
        to += incr * chunk;
        size -= chunk;
    }

    // L2 mappings
    chunk = ALIGN_DOWN(size, MASK(VADDR_L2_OFFSET_BITS));
    if (chunk && (!hw || (to & VADDR_L2_ALIGN_MASK) == 0)) {
        hv_pt_map_l2(from, to, chunk, incr);
        from += chunk;
        to += incr * chunk;
        size -= chunk;
    }

    // L3 mappings to end
    chunk = ALIGN_DOWN(size, MASK(VADDR_L3_OFFSET_BITS));
    if (chunk) {
        hv_pt_map_l3(from, to, chunk, incr);
        from += chunk;
        to += incr * chunk;
        size -= chunk;
    }

    // L4 mappings to end
    if (size) {
        assert(!hw);
        hv_pt_map_l4(from, to, size, incr);
    }

    return 0;
}

int hv_unmap(u64 from, u64 size)
{
    return hv_map(from, 0, size, 0);
}

int hv_map_hw(u64 from, u64 to, u64 size)
{
    return hv_map(from, to | PTE_ATTRIBUTES | PTE_VALID, size, 1);
}

int hv_map_sw(u64 from, u64 to, u64 size)
{
    return hv_map(from, to | FIELD_PREP(SPTE_TYPE, SPTE_MAP), size, 1);
}

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    return hv_map(from, ((u64)hook) | FIELD_PREP(SPTE_TYPE, SPTE_HOOK), size, 0);
}

u64 hv_translate(u64 addr, bool s1, bool w)
{
    if (!(mrs(SCTLR_EL12) & SCTLR_M))
        return addr; // MMU off

    u64 el = FIELD_GET(SPSR_M, mrs(SPSR_EL2)) >> 2;
    u64 save = mrs(PAR_EL1);

    if (w) {
        if (s1) {
            if (el == 0)
                asm("at s1e0w, %0" : : "r"(addr));
            else
                asm("at s1e1w, %0" : : "r"(addr));
        } else {
            if (el == 0)
                asm("at s12e0w, %0" : : "r"(addr));
            else
                asm("at s12e1w, %0" : : "r"(addr));
        }
    } else {
        if (s1) {
            if (el == 0)
                asm("at s1e0r, %0" : : "r"(addr));
            else
                asm("at s1e1r, %0" : : "r"(addr));
        } else {
            if (el == 0)
                asm("at s12e0r, %0" : : "r"(addr));
            else
                asm("at s12e1r, %0" : : "r"(addr));
        }
    }

    u64 par = mrs(PAR_EL1);
    msr(PAR_EL1, save);

    if (par & PAR_F) {
        dprintf("hv_translate(0x%lx, %d, %d): fault 0x%lx\n", addr, s1, w, par);
        return 0; // fault
    } else {
        return (par & PAR_PA) | (addr & 0xfff);
    }
}

u64 hv_pt_walk(u64 addr)
{
    dprintf("hv_pt_walk(0x%lx)\n", addr);

    u64 idx = addr >> VADDR_L2_OFFSET_BITS;
    u64 l2d = hv_L2[idx];

    dprintf("  l2d = 0x%lx\n", l2d);

    if (!L2_IS_TABLE(l2d)) {
        if (L2_IS_SW_BLOCK(l2d) || L2_IS_HW_BLOCK(l2d))
            l2d |= addr & (VADDR_L2_ALIGN_MASK | VADDR_L3_ALIGN_MASK);

        dprintf("  result: 0x%lx\n", l2d);
        return l2d;
    }

    idx = (addr >> VADDR_L3_OFFSET_BITS) & MASK(VADDR_L3_INDEX_BITS);
    u64 l3d = ((u64 *)(l2d & PTE_TARGET_MASK))[idx];
    dprintf("  l3d = 0x%lx\n", l3d);

    if (!L3_IS_TABLE(l3d)) {
        if (L3_IS_SW_BLOCK(l3d))
            l3d |= addr & VADDR_L3_ALIGN_MASK;
        if (L3_IS_HW_BLOCK(l3d)) {
            l3d &= ~PTE_LOWER_ATTRIBUTES;
            l3d |= addr & VADDR_L3_ALIGN_MASK;
        }
        dprintf("  result: 0x%lx\n", l3d);
        return l3d;
    }

    idx = (addr >> VADDR_L4_OFFSET_BITS) & MASK(VADDR_L4_INDEX_BITS);
    dprintf("  l4 idx = 0x%lx\n", idx);
    u64 l4d = ((u64 *)(l3d & PTE_TARGET_MASK))[idx];
    dprintf("  l4d = 0x%lx\n", l4d);
    return l4d;
}

#define CHECK_WIDTH                                                                                \
    if ((insn >> 30) != width)                                                                     \
    goto bad_width
#define CHECK_RN                                                                                   \
    if (Rn == 31)                                                                                  \
    goto bail
#define CHECK_RT                                                                                   \
    if (Rt == 31)                                                                                  \
    return true

#define EXT(n, b) (((s32)(((u32)(n)) << (32 - (b)))) >> (32 - (b)))

static bool emulate_load(u64 *regs, u32 insn, u64 val, u64 width)
{
    u64 Rt = insn & 0x1f;
    u64 Rn = (insn >> 5) & 0x1f;
    u64 imm9 = EXT((insn >> 12) & 0x1ff, 9);

    if ((insn & 0x3fe00400) == 0x38400400) {
        // LDRx (immediate) Pre/Post-index
        CHECK_WIDTH;
        CHECK_RN;
        regs[Rn] += imm9;
        CHECK_RT;
        regs[Rt] = val;
    } else if ((insn & 0x3fc00000) == 0x39400000) {
        // LDRx (immediate) Unsigned offset
        CHECK_WIDTH;
        CHECK_RT;
        regs[Rt] = val;
    } else if ((insn & 0x3fa00400) == 0x38800400) {
        // LDRSx (immediate) Pre/Post-index
        CHECK_WIDTH;
        CHECK_RN;
        regs[Rn] += imm9;
        CHECK_RT;
        regs[Rt] = EXT(val, 8 << width);
    } else if ((insn & 0x3fa00000) == 0x39800000) {
        // LDRSx (immediate) Unsigned offset
        CHECK_WIDTH;
        CHECK_RT;
        regs[Rt] = EXT(val, 8 << width);
    } else if ((insn & 0x3fe04c00) == 0x38604800) {
        // LDRx (register)
        CHECK_WIDTH;
        CHECK_RT;
        regs[Rt] = val;
    } else if ((insn & 0x3fa04c00) == 0x38a04800) {
        // LDRSx (register)
        CHECK_WIDTH;
        CHECK_RT;
        regs[Rt] = EXT(val, 8 << width);
    } else {
        goto bail;
    }
    return true;

bad_width:
    printf("HV: width mismatch (expected %ld)\n", width);
bail:
    printf("HV: load not emulated: 0x%08x\n", insn);
    return false;
}

static bool emulate_store(u64 *regs, u32 insn, u64 *val, u64 width)
{
    u64 Rt = insn & 0x1f;
    u64 Rn = (insn >> 5) & 0x1f;
    u64 imm9 = EXT((insn >> 12) & 0x1ff, 9);

    if ((insn & 0x3fe00400) == 0x38000400) {
        // STRx (immediate) Pre/Post-index
        CHECK_WIDTH;
        CHECK_RN;
        regs[Rn] += imm9;
        CHECK_RT;
        *val = regs[Rt];
    } else if ((insn & 0x3fc00000) == 0x39000000) {
        // STRx (immediate) Unsigned offset
        CHECK_WIDTH;
        CHECK_RT;
        *val = regs[Rt];
    } else if ((insn & 0x3fe04c00) == 0x38204800) {
        // STRx (register)
        CHECK_WIDTH;
        CHECK_RT;
        *val = regs[Rt];
    } else {
        goto bail;
    }
    return true;

bad_width:
    printf("HV: width mismatch (expected %ld)\n", width);
bail:
    printf("HV: store not emulated: 0x%08x\n", insn);
    return false;
}

bool hv_handle_dabort(u64 *regs)
{
    u64 esr = mrs(ESR_EL2);

    if (!(esr & ESR_ISS_DABORT_ISV))
        return false;

    u64 far = mrs(FAR_EL2);
    u64 ipa = hv_translate(far, true, esr & ESR_ISS_DABORT_WnR);

    dprintf("hv_handle_abort(): stage 1 0x%0lx -> 0x%lx\n", far, ipa);

    if (!ipa)
        return false;

    u64 pte = hv_pt_walk(ipa);

    if (!pte)
        return false;

    if (IS_HW(pte)) {
        printf("HV: Data abort on mapped page (0x%lx -> 0x%lx\n", far, pte);
        return false;
    }

    assert(IS_SW(pte));

    u64 target = pte & PTE_TARGET_MASK_L4;
    u64 paddr = target | (far & MASK(VADDR_L4_OFFSET_BITS));

    u64 width = FIELD_GET(ESR_ISS_DABORT_SAS, esr);

    u64 elr = mrs(ELR_EL2);
    u64 elr_pa = hv_translate(elr, false, false);
    if (!elr_pa) {
        printf("HV: Failed to fetch instruction for data abort at 0x%lx\n", elr);
        return false;
    }

    u32 insn = read32(elr_pa);
    u64 val = 0;

    if (esr & ESR_ISS_DABORT_WnR) {
        if (!emulate_store(regs, insn, &val, width))
            return false;

        switch (FIELD_GET(SPTE_TYPE, pte)) {
            case SPTE_MAP:
                dprintf("HV: SPTE_MAP[W] 0x%lx -> 0x%lx (w=%d): 0x%lx\n", far, paddr, 1 << width,
                        val);
                switch (width) {
                    case SAS_8B:
                        write8(paddr, val);
                        break;
                    case SAS_16B:
                        write16(paddr, val);
                        break;
                    case SAS_32B:
                        write32(paddr, val);
                        break;
                    case SAS_64B:
                        write64(paddr, val);
                        break;
                }
                break;
            case SPTE_HOOK: {
                hv_hook_t *hook = (hv_hook_t *)target;
                if (!hook(ipa, &val, true, width))
                    return false;
                dprintf("HV: SPTE_HOOK[W] 0x%lx -> 0x%lx (w=%d) @%p: 0x%lx\n", far, ipa, 1 << width,
                        hook, val);
                break;
            }
        }
    } else {
        switch (FIELD_GET(SPTE_TYPE, pte)) {
            case SPTE_MAP:
                switch (width) {
                    case SAS_8B:
                        val = read8(paddr);
                        break;
                    case SAS_16B:
                        val = read16(paddr);
                        break;
                    case SAS_32B:
                        val = read32(paddr);
                        break;
                    case SAS_64B:
                        val = read64(paddr);
                        break;
                }
                dprintf("HV: SPTE_MAP[R] 0x%lx -> 0x%lx (w=%d): 0x%lx\n", far, paddr, 1 << width,
                        val);
                break;
            case SPTE_HOOK:
                val = 0;
                hv_hook_t *hook = (hv_hook_t *)target;
                if (!hook(ipa, &val, false, width))
                    return false;
                dprintf("HV: SPTE_HOOK[R] 0x%lx -> 0x%lx (w=%d) @%p: 0x%lx\n", far, ipa, 1 << width,
                        hook, val);
                break;
        }

        if (!emulate_load(regs, insn, val, width))
            return false;
    }

    msr(ELR_EL2, elr + 4);
    return true;
}
