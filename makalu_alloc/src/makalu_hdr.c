#include "makalu_internal.h"


/*
 * This implements:
 * 1. allocation of heap block headers
 * 2. A map from addresses to heap block addresses to heap block headers
 *
 * Access speed is crucial.  We implement an index structure based on a 2
 * level tree.
 */

MAK_INNER bottom_index* MAK_all_nils = NULL;
MAK_INNER bottom_index** MAK_top_index;



MAK_INNER hdr * MAK_find_header(ptr_t h)
{
        hdr * result;
        GET_HDR(h, result);
        return(result);
}

/* Handle a header cache miss.  Returns a pointer to the        */
/* header corresponding to p, if p can possibly be a valid      */
/* object pointer, and 0 otherwise.                             */
/* GUARANTEED to return 0 for a pointer past the first page     */
/* of an object unless both GC_all_interior_pointers is set     */
/* and p is in fact a valid object pointer.                     */
/* Never returns a pointer to a free hblk.                      */

hdr_cache_entry MAK_hdr_cache[HDR_CACHE_SIZE];

MAK_INNER hdr * MAK_header_cache_miss(ptr_t p, hdr_cache_entry *hce)
{
  hdr *hhdr;
  GET_HDR(p, hhdr);
  if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
    if (MAK_all_interior_pointers) {
      if (hhdr != 0) {
        ptr_t current = p;

        current = (ptr_t)HBLKPTR(current);
        do {
            current = current - HBLKSIZE*(word)hhdr;
            hhdr = HDR(current);
        } while(IS_FORWARDING_ADDR_OR_NIL(hhdr));
        /* current points to near the start of the large object */
        if (hhdr -> hb_flags & IGNORE_OFF_PAGE)
            return 0;
        if (HBLK_IS_FREE(hhdr)
            || p - current >= (ptrdiff_t)(hhdr->hb_sz)) {
            /* Pointer past the end of the block */
            return 0;
        }
      } 
      MAK_ASSERT(hhdr == 0 || !HBLK_IS_FREE(hhdr));
      return hhdr;
      /* Pointers past the first page are probably too rare     */
      /* to add them to the cache.  We don't.                   */
      /* And correctness relies on the fact that we don't.      */
    } else {
      return 0;
    }
  } else {
    if (HBLK_IS_FREE(hhdr)) {
      return 0;
    } else {
      hce -> block_addr = (word)(p) >> LOG_HBLKSIZE;
      hce -> hce_hdr = hhdr;
      return hhdr;
    }
  }
}

MAK_INNER void MAK_update_hc(ptr_t p, hdr* hhdr, hdr_cache_entry* hc, word hc_sz){
    hdr_cache_entry* entry = HCE(p, hc, hc_sz);
    entry -> block_addr = ((word) (p)) >> LOG_HBLKSIZE;
    //if (HBLKPTR(p) != hhdr -> hb_block){
    //    GC_printf("hhdr block: %p, mismatched HBLKPTR(p): %p\n", hhdr -> hb_block, p);
    //}
    entry -> hce_hdr = hhdr;
}

MAK_INNER hdr* MAK_get_hdr_no_update(ptr_t p, hdr_cache_entry* hc, word hc_sz){
     hdr_cache_entry * hce = HCE(p, hc, hc_sz);
     hdr* hhdr;
     if (EXPECT(HCE_VALID_FOR(hce, p), TRUE)) {
         hhdr = hce -> hce_hdr;
         if (EXPECT(hhdr -> hb_block == HBLKPTR(p), TRUE)){
             //GC_printf("Header found in cache\n");
             return hhdr;
         }
     }
     return HDR(p);
}

MAK_INNER hdr* MAK_get_hdr_and_update_hc(ptr_t p, hdr_cache_entry* hc, word hc_sz){
     hdr_cache_entry * hce = HCE(p, hc, hc_sz);
     hdr* hhdr;
     if (EXPECT(HCE_VALID_FOR(hce, p), TRUE)) {
         hhdr = hce -> hce_hdr;
         if (EXPECT(hhdr -> hb_block == HBLKPTR(p), TRUE)){
             //GC_printf("Header found in cache\n");
             return hhdr;
         }
     }
     hhdr = HDR(p);
     //if (HBLKPTR(p) != hhdr -> hb_block){
     // GC_printf("hhdr block: %p, mismatched HBLKPTR(p): %p\n", hhdr -> hb_block, p);
     //}

     hce -> block_addr = ((word)(hhdr -> hb_block)) >> LOG_HBLKSIZE;
     hce -> hce_hdr = hhdr;
     return hhdr;
}

static ptr_t MAK_hdr_end_ptr = NULL;
static word MAK_curr_hdr_space = 0;
static ptr_t MAK_hdr_idx_end_ptr = NULL;
static word MAK_curr_hdr_idx_space = 0;

static inline MAK_bool scratch_alloc_hdr_space(word bytes_to_get)
{
    word i;
    word next;

    //we have some memory to use;
    if (MAK_curr_hdr_space < MAK_n_hdr_spaces){
         i = MAK_curr_hdr_space;
         goto out;
    }

    if (MAK_n_hdr_spaces >= MAX_HEAP_SECTS)
        ABORT("Max heap sectors reached! Cannot allocate any more header for the heap pages!");
    i = MAK_n_hdr_spaces;
    MAK_STORE_NVM_SYNC(MAK_n_hdr_spaces, MAK_n_hdr_spaces + 1);
    MAK_STORE_NVM_SYNC(MAK_hdr_spaces[i].hs_bytes, bytes_to_get);
    int res = GET_MEM_PERSISTENT(&(MAK_hdr_spaces[i].hs_start), bytes_to_get);
    if (res != 0){
        MAK_STORE_NVM_SYNC(MAK_n_hdr_spaces, i);
        WARN("Could not acquire space for headers!\n", 0);
        return FALSE;
    }
out:
    //we initialize the free pointer to point to the 
    //proper sector on restart, when the first header is allocated
    //in that sector, we guarantee the free pointer to be visible in nvram at the end of the correspoding transaction
    MAK_hdr_free_ptr = MAK_hdr_spaces[i].hs_start;
    MAK_hdr_end_ptr = MAK_hdr_free_ptr + bytes_to_get;
    return TRUE;
}

static hdr* alloc_hdr(void)
{
    register hdr* result;
    int attempt;
    if (GC_hdr_free_list != 0)
    {
        result = GC_hdr_free_list;
        //incase of a crash, we rebuild the freelist, 
        //so it is ok to be out of sync with the transaction
        GC_hdr_free_list = (hdr *) (result -> hb_next);
        result->hb_prev = result->hb_next = 0;
        //GC_printf("Hdr being allocated: %p\n", result);
        //headers in the header freelist have all size set to zero, 
        //but we need to log it so that
        //undoing of a transaction can leave it size = 0, 
        //and we can add to freelist when scanning
        GC_LOG_NVM_WORD(&(result->hb_sz), 0);
        return result;
    }

    result = (hdr*) GC_hdr_free_ptr;
    ptr_t new_free_ptr = GC_hdr_free_ptr + sizeof (hdr);
    if (new_free_ptr <= GC_hdr_end_ptr){
        //it is important to be in sync with the transaction, 
        //because that upto where we scan to for headers
        GC_NO_LOG_STORE_NVM(GC_hdr_free_ptr, new_free_ptr);
        //headers size does not need to be set to zero because the 
        //free_ptr would be rewinded in case of a crash
        //GC_printf("%dth header allocated\n", hdr_alloc_count);
        return (result);
    }
    //we expand the hdr scratch space based on the 
    //size of previous heap expansion size
    word bytes_to_get = MINHINCR * HBLKSIZE;
    if (GC_last_heap_size > 0){
        word h_blocks = divHBLKSZ(GC_last_heap_size);
        word btg = (OBJ_SZ_TO_BLOCKS(h_blocks * sizeof(hdr))) * HBLKSIZE;
        if (btg > bytes_to_get) bytes_to_get = btg;
    }
    GC_curr_hdr_space++;
    GC_bool res = scratch_alloc_hdr_space(bytes_to_get);
    if (!res && bytes_to_get > MINHINCR * HBLKSIZE)
        res =  scratch_alloc_hdr_space(MINHINCR * HBLKSIZE);
    if (!res)
        res =  scratch_alloc_hdr_space(HBLKSIZE);
    if (!res)
        ABORT("Could not acquire even a page for header space!\n");
    return alloc_hdr();
}

static inline GC_bool scratch_alloc_hdr_idx_space(word bytes_to_get)
{
    word i;

    if (GC_curr_hdr_idx_space < GC_n_hdr_idx_spaces) {
        i = GC_curr_hdr_idx_space;
        goto out;
    }

    if (GC_n_hdr_idx_spaces >= MAX_HEAP_SECTS)
        ABORT("Max heap sectors reached! Cannot create header index for the heap pages!");
    i = GC_n_hdr_idx_spaces;
    GC_STORE_NVM_SYNC(GC_n_hdr_idx_spaces, GC_n_hdr_idx_spaces + 1);
    GC_STORE_NVM_SYNC(GC_hdr_idx_spaces[i].hs_bytes, bytes_to_get);
    int res = GET_MEM_PERSISTENT(&(GC_hdr_idx_spaces[i].hs_start), bytes_to_get);
    if (res != 0){
        GC_STORE_NVM_SYNC(GC_n_hdr_idx_spaces, i);
        WARN("Could not acquire space for header indices!\n", 0);
        return FALSE;
    }
out:
    //no need to be synchronous to the computation here, 
    //we rebuild it from scratch in the case of a crash
    GC_hdr_idx_free_ptr = GC_hdr_idx_spaces[i].hs_start;
    GC_hdr_idx_end_ptr = GC_hdr_idx_free_ptr + bytes_to_get;
    return TRUE;
}

static bottom_index* alloc_bi(void)
{
    register bottom_index* result;

    result = (bottom_index*) GC_hdr_idx_free_ptr;
    ptr_t new_free_ptr = GC_hdr_idx_free_ptr + sizeof (bottom_index);
    if (new_free_ptr <= GC_hdr_idx_end_ptr){
        //incase of a crash we reset the free_ptr
        GC_hdr_idx_free_ptr = new_free_ptr;
        //GC_printf("%dth bottom index allocated\n", alloc_bi_count);
        return (result);
    }
    word bytes_to_get = MINHINCR * HBLKSIZE;
    GC_curr_hdr_idx_space++;
    GC_bool res = scratch_alloc_hdr_idx_space(bytes_to_get);
    if (!res)
        res =  scratch_alloc_hdr_idx_space(HBLKSIZE);
    if (!res)
        ABORT("Could not even acquire a page for header indices!\n");
    return alloc_bi();
}

GC_INNER void GC_init_headers(void)
{
    //all visibility concerns here are addressed
    //allocate initial header space
    word bytes_to_get = MINHINCR * HBLKSIZE;
    GC_curr_hdr_space = 0;
    GC_n_hdr_spaces = 0;
    scratch_alloc_hdr_space(bytes_to_get);

    //allocate initial header index space
    bytes_to_get = MINHINCR * HBLKSIZE;
    GC_curr_hdr_idx_space = 0;
    GC_n_hdr_idx_spaces = 0;
    scratch_alloc_hdr_idx_space(bytes_to_get);

    register unsigned i;
    for (i = 0; i < TOP_SZ; i++) {
        GC_top_index[i] = GC_all_nils;
    }
}

GC_INNER void GC_remove_header(struct hblk *h)
{
    hdr **ha;
    GET_HDR_ADDR(h, ha);
    //free_hdr(*ha);
    hdr* hhdr = *ha;
    //we don't log because whoever called this to be removed has already logged the appropriate fields in the header
    //e.g. GC_newfreehblk calls GC_remove_from_fl before it calls this function which makes sure that hb_prev, hb_next, and hb_sz is logged
    GC_NO_LOG_STORE_NVM(hhdr->hb_sz, 0);
  #ifdef FIXED_SIZE_GLOBAL_FL
    //The below is necessary for header cache to work properly
    GC_STORE_NVM_ADDR(&(hhdr -> hb_block), 0);
  #endif
    GC_STORE_NVM_ASYNC(hhdr -> hb_next, (struct hblk*) GC_hdr_free_list);
    //will be made visible before exit
    GC_hdr_free_list = hhdr;
    GC_STORE_NVM_PTR_ASYNC(ha, 0);
}

/* Make sure that there is a bottom level index block for address addr  */
/* Return FALSE on failure.                                             */
static GC_bool get_index(word addr)
{
    word hi = (word)(addr) >> (LOG_BOTTOM_SZ + LOG_HBLKSIZE);
    bottom_index * r;
    bottom_index * p;
    bottom_index ** prev;
    bottom_index *pi;

    word i = TL_HASH(hi);
    bottom_index * old;

    old = p = GC_top_index[i];
    while(p != GC_all_nils) {
        if (p -> key == hi) return(TRUE);
        p = p -> hash_link;
    }
    r = alloc_bi();
    if (r == 0) return(FALSE);
    BZERO(r, sizeof (bottom_index));
    r -> hash_link = old;
    //no need to log changes to r because in case of failure r. 
    //Relies on the original value of GC_scratch_free_ptr being logged before the
    //start of any allocation
    //during GC_free, the GC_top_index is read without a lock held. So, we have to
    //ensure that the write is a 64-bit write instead of 2 32 bytes write which compiler
    //can generate
    ENSURE_64_BIT_COPY(GC_top_index[i], r);
    GC_NVM_ASYNC_RANGE(&(GC_top_index[i]), sizeof(bottom_index*));
    r -> key = hi;
    /* Add it to the list of bottom indices */
    prev = &GC_all_bottom_indices;    /* pointer to p */
    pi = 0;                           /* bottom_index preceding p */
    while ((p = *prev) != 0 && p -> key < hi) {
      pi = p;
      prev = &(p -> asc_link);
    }
    r -> desc_link = pi;
    if (0 == p) {
      GC_STORE_NVM_ASYNC(GC_all_bottom_indices_end, r);
    } else {
      GC_STORE_NVM_ASYNC(p -> desc_link, r);
    }
    r -> asc_link = p;
    //*prev = r;
    GC_STORE_NVM_PTR_ASYNC(prev, r);
    GC_NVM_ASYNC_RANGE(r, sizeof(bottom_index));
    return(TRUE);
}

GC_INNER void GC_restart_persistent_scratch_alloc(){
    int  i;
    ptr_t end;
    ptr_t start;
    ptr_t free_ptr = GC_hdr_free_ptr;
    i = (int) GC_n_hdr_spaces - 1;
    //last memory acquisition was incomplete
    if (GC_hdr_spaces[i].hs_start == NULL){
        //don't really need to flush it. We will get to it next time
        //we do need to fix it.
        GC_STORE_NVM_ASYNC(GC_n_hdr_spaces, GC_n_hdr_spaces - 1);
        i--;
    }

    //if we allocated some memory for header but died before doing any allocation from it
    if (free_ptr == 0)
    {
        i = 0;
        GC_curr_hdr_space = 0;
        GC_hdr_free_ptr = GC_hdr_spaces[i].hs_start;
        GC_hdr_end_ptr = GC_hdr_free_ptr + GC_hdr_spaces[i].hs_bytes;
    }
    else { //if not we have a valid free_ptr
        GC_curr_hdr_space = GC_n_hdr_spaces;
        GC_hdr_end_ptr = 0;
        for ( ; i >= 0; i--){
            start = GC_hdr_spaces[i].hs_start;
            end = start + GC_hdr_spaces[i].hs_bytes;
            if (free_ptr >= start && free_ptr < end){
                GC_curr_hdr_space = (word) i;
                GC_hdr_end_ptr = end;
                break;
            }
       }
    }

    free_ptr = GC_hdr_idx_free_ptr;
    i = (int) GC_n_hdr_idx_spaces - 1;
    //last memory acquisition was incomplete
    if (GC_hdr_idx_spaces[i].hs_start == NULL){
       GC_STORE_NVM_ASYNC(GC_n_hdr_idx_spaces, GC_n_hdr_idx_spaces - 1);
       i--;
    }

    //no need to restart the idx allocation since it has to be rebuild anyway
    if (GC_persistent_state != PERSISTENT_STATE_NONE)
        return;


    //see the comment for headers
    if (free_ptr == 0)
    {
        i = 0;
        GC_curr_hdr_idx_space = 0;
        GC_hdr_idx_free_ptr = GC_hdr_idx_spaces[i].hs_start;
        GC_hdr_idx_end_ptr = GC_hdr_idx_free_ptr + GC_hdr_idx_spaces[i].hs_bytes;
    }
    else
    {
        GC_curr_hdr_idx_space = GC_n_hdr_idx_spaces;
        GC_hdr_idx_end_ptr = 0;
        for ( ; i >=0; i--){
            start = GC_hdr_idx_spaces[i].hs_start;
            end = start + GC_hdr_idx_spaces[i].hs_bytes;
            if (free_ptr >= start && free_ptr < end){
                GC_curr_hdr_idx_space = (word) i;
                GC_hdr_idx_end_ptr = end;
                break;
           }
        }
    }
}

GC_INNER void GC_rebuild_metadata_from_headers()
{
    //rewind the header_idx space
    GC_hdr_idx_free_ptr = NULL;
    GC_curr_hdr_idx_space = 0;
    GC_hdr_idx_end_ptr = NULL;
    GC_all_bottom_indices_end = 0;
    GC_all_bottom_indices = 0;
    if (!scratch_alloc_hdr_idx_space(MINHINCR * HBLKSIZE))
        ABORT("Metadata rebuild unsuccessful: Could not allocate header idx space!\n");

    register unsigned i;
    for (i = 0; i < TOP_SZ; i++) {
        GC_STORE_NVM_ASYNC(GC_top_index[i], GC_all_nils);
    }

    word space = GC_n_hdr_spaces - 1;
    //fix the last unsuccessful acquisition
    if (GC_hdr_spaces[space].hs_start == NULL){
        //don't really need to flush it. We will get to it next time
        //we do need to fix it.
        GC_STORE_NVM_ASYNC(GC_n_hdr_spaces, GC_n_hdr_spaces - 1);
    }

    space = GC_n_hdr_idx_spaces - 1;

    //last memory acquisition was incomplete
    if (GC_hdr_idx_spaces[space].hs_start == NULL){
       GC_STORE_NVM_ASYNC(GC_n_hdr_idx_spaces, GC_n_hdr_idx_spaces - 1);
    }

    hdr* hdr_fl = NULL;
    struct hblk* hfreelist[N_HBLK_FLS+1];
    BZERO(hfreelist, sizeof(struct hblk*) * (N_HBLK_FLS + 1));

    //word free_bytes[N_HBLK_FLS+1];
    //BZERO(free_bytes, sizeof(word) * (N_HBLK_FLS + 1));

    register hdr* curr;
    ptr_t end;
    struct hblk* h;
    struct hblk* second;
    hdr* second_hdr;
    word sz;
    word n_blocks;
    int fl_index;
    //int count = 0; 
    for(space = 0; space < GC_n_hdr_spaces; space++)
    {
        curr = (hdr*) GC_hdr_spaces[space].hs_start;
        end  = ( (ptr_t) curr ) + GC_hdr_spaces[space].hs_bytes;
        if (GC_hdr_free_ptr >= (ptr_t) curr && GC_hdr_free_ptr < end)
            end = GC_hdr_free_ptr;
        for (; (((ptr_t) curr) + sizeof(hdr)) <= end; curr++){
            sz = curr -> hb_sz;

            //GC_printf("Scanning header %d\n", count);
            //count++;
            //if (count == 1172)
            //{
            //   count = count + 1 - 1;
            //   GC_printf("Happy to die!\n");
            //}

            //if its an unallocated header 
            if (sz == 0){
               GC_STORE_NVM_ASYNC(curr -> hb_next, (struct hblk*) hdr_fl);
               //curr->hb_next = (struct hblk*) hdr_fl;
               hdr_fl = curr;
               continue;
            }

            h = curr -> hb_block;
            //if (h == 0)
            //   i("FOUND an allocated header with no block addr\n");

            if (!get_index((word)h)){
                goto out;
            }
            SET_HDR(h, curr);
            //if its a free block
            if (HBLK_IS_FREE(curr)){
                continue;
            }
            //its an allocated block
            //install counts
            sz = HBLKSIZE * OBJ_SZ_TO_BLOCKS(sz);

            for (second = h + BOTTOM_SZ; 
             (char*) second < (char*)h + sz; 
             second += BOTTOM_SZ) {
                if (!get_index((word)second)) goto out;
            }
            if (!get_index((word)h + sz - 1))
                goto out;
            for (second = h + 1; (char*) second < (char*) h + sz; second += 1) {
                n_blocks = HBLK_PTR_DIFF(second, h);
                SET_HDR(second, (hdr *)(n_blocks > MAX_JUMP? MAX_JUMP : n_blocks));
            }
        }
    }
    //No need to flush here. Will be flushed by GC_sync_alloc_metadata
    GC_hdr_free_list = hdr_fl;

    return;

out:
   ABORT("Could not install the index for a header!\n");
}

/* Install a header for block h.        */
/* The header is uninitialized.         */
/* Returns the header or 0 on failure.  */
GC_INNER struct hblkhdr * GC_install_header(struct hblk *h)
{
    hdr * result;

    if (!get_index((word) h)) return(0);
    result = alloc_hdr();
    //we need this information in every header to 
    //process the headers in the case of a crash
    GC_NO_LOG_STORE_NVM(result->hb_block, h);

    if (result) {
      SET_HDR(h, result);
    }
    return(result);
}

/* Set up forwarding counts for block h of size sz */
GC_INNER GC_bool GC_install_counts(struct hblk *h, size_t sz/* bytes */)
{
    struct hblk * hbp;
    word i;

    for (hbp = h; (char *)hbp < (char *)h + sz; hbp += BOTTOM_SZ) {
        if (!get_index((word) hbp)) return(FALSE);
    }
    if (!get_index((word)h + sz - 1)) return(FALSE);
    for (hbp = h + 1; (char *)hbp < (char *)h + sz; hbp += 1) {
        i = HBLK_PTR_DIFF(hbp, h);
        SET_HDR(hbp, (hdr *)(i > MAX_JUMP? MAX_JUMP : i));
    }
    return(TRUE);
}

/* Remove forwarding counts for h */
GC_INNER void GC_remove_counts(struct hblk *h, size_t sz/* bytes */)
{
    register struct hblk * hbp;
    for (hbp = h+1; (char *)hbp < (char *)h + sz; hbp += 1) {
        SET_HDR(hbp, 0);
    }
}

/* Apply fn to all allocated blocks */
/*VARARGS1*/
void GC_apply_to_all_blocks(void (*fn)(struct hblk *h, word client_data),
                            word client_data)
{
    signed_word j;
    bottom_index * index_p;

    for (index_p = GC_all_bottom_indices; index_p != 0;
         index_p = index_p -> asc_link) {
        for (j = BOTTOM_SZ-1; j >= 0;) {
            if (!IS_FORWARDING_ADDR_OR_NIL(index_p->index[j])) {
                if (!HBLK_IS_FREE(index_p->index[j])) {
                    (*fn)(((struct hblk *)
                              (((index_p->key << LOG_BOTTOM_SZ) + (word)j)
                               << LOG_HBLKSIZE)),
                          client_data);
                }
                j--;
             } else if (index_p->index[j] == 0) {
                j--;
             } else {
                j -= (signed_word)(index_p->index[j]);
             }
         }
     }
}

/* Get the next valid block whose address is at least h */
/* Return 0 if there is none.                           */
GC_INNER struct hblk * GC_next_used_block(struct hblk *h)
{
    register bottom_index * bi;
    register word j = ((word)h >> LOG_HBLKSIZE) & (BOTTOM_SZ-1);

    GET_BI(h, bi);
    if (bi == GC_all_nils) {
        register word hi = (word)h >> (LOG_BOTTOM_SZ + LOG_HBLKSIZE);
        bi = GC_all_bottom_indices;
        while (bi != 0 && bi -> key < hi) bi = bi -> asc_link;
        j = 0;
    }
    while(bi != 0) {
        while (j < BOTTOM_SZ) {
            hdr * hhdr = bi -> index[j];
            if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
                j++;
            } else {
                if (!HBLK_IS_FREE(hhdr)) {
                    return((struct hblk *)
                              (((bi -> key << LOG_BOTTOM_SZ) + j)
                               << LOG_HBLKSIZE));
                } else {
                    j += divHBLKSZ(hhdr -> hb_sz);
                }
            }
        }
        j = 0;
        bi = bi -> asc_link;
    }
    return(0);
}

/* Get the last (highest address) block whose address is        */
/* at most h.  Return 0 if there is none.                       */
/* Unlike the above, this may return a free block.              */
GC_INNER struct hblk * GC_prev_block(struct hblk *h)
{
    register bottom_index * bi;
    register signed_word j = ((word)h >> LOG_HBLKSIZE) & (BOTTOM_SZ-1);

    GET_BI(h, bi);
    if (bi == GC_all_nils) {
        register word hi = (word)h >> (LOG_BOTTOM_SZ + LOG_HBLKSIZE);
        bi = GC_all_bottom_indices_end;
        while (bi != 0 && bi -> key > hi) bi = bi -> desc_link;
        j = BOTTOM_SZ - 1;
    }
    while(bi != 0) {
        while (j >= 0) {
            hdr * hhdr = bi -> index[j];
            if (0 == hhdr) {
                --j;
            } else if (IS_FORWARDING_ADDR_OR_NIL(hhdr)) {
                j -= (signed_word)hhdr;
            } else {
                return((struct hblk *)
                          (((bi -> key << LOG_BOTTOM_SZ) + j)
                               << LOG_HBLKSIZE));
            }
        }
        j = BOTTOM_SZ - 1;
        bi = bi -> desc_link;
    }
    return(0);
}

