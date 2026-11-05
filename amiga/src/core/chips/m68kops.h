#ifndef M68KOPS__HEADER
#define M68KOPS__HEADER

#ifdef __cplusplus
extern "C" {
#endif

extern unsigned char (*m68ki_cycles)[0x10000];
extern unsigned short *m68ki_instruction_index;
extern void (*m68ki_handlers[])(void);
void m68ki_build_opcode_table(void);

#ifdef __cplusplus
}
#endif

#endif /* M68KOPS__HEADER */
