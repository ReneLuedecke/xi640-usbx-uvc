/* DCMIPP Frame Buffer Section in AXISRAM1 @ 0x34000000 (first 1.5 MB free)
 *
 * Speicherbelegung (BPP=2, 614400 Bytes/Frame):
 *   frame_buf_a: 0x34000000 .. 0x34095FFF  (614400 Bytes)
 *   frame_buf_b: 0x34096000 .. 0x3412BFFF  (614400 Bytes)
 *   Code-Start:  0x34180400                (337 KB Abstand — kein Overlap!)
 *
 * Subsection-Reihenfolge: .dcmipp_frames.a VOR .dcmipp_frames.b
 * Verhindert SORT_BY_ALIGNMENT-Vertauschung durch Zephyr-Linker-Makros.
 *
 * DMA write target — D-Cache invalidate required after video_dequeue()
 */
SECTION_DATA_PROLOGUE(.dcmipp_frames,(NOLOAD),)
{
    . = ALIGN(64);
    *(.dcmipp_frames.a)   /* frame_buf_a — immer zuerst */
    *(.dcmipp_frames.b)   /* frame_buf_b — immer zweiter */
    *(.dcmipp_frames)     /* Fallback für andere Symbole */
    . = ALIGN(64);
} GROUP_DATA_LINK_IN(AXISRAM1, AXISRAM1)
