                        page    ,132
                        title   Utility Functions

                        .code

                        ; uintptr_t __fastcall __readbp(void);
                        public __readbp
__readbp                proc
                        mov     rax, rbp
                        ret
__readbp                endp

                        end


