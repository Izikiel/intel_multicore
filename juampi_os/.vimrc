"" Highlight types
autocmd BufRead,BufNewFile *.[ch] let fname = expand('<afile>:p:h') . '/../types.vim'
autocmd BufRead,BufNewFile *.[ch] if filereadable(fname)
autocmd BufRead,BufNewFile *.[ch]	exe 'so ' . fname
autocmd BufRead,BufNewFile *.[ch] endif 
