# Retrocomputing tools ported under Linux by Claude
This is a collection of existing retrocomputing tools, mainly existing under MS-DOS/Windows and ported
To Linux/Unix. Debian 12 was used as development platform, but since from Debian 10 raw FDD commands support
is dropped, may need to be built under older linux releases

## APRIDISK
Started disassembling the original executable and trying reconstructing the original code in Borland C++
Proceeded to generate a C equivalent code starting from assembly, with its own makefile

Still untested in real fdd writing it compiles fine and reads apridisk dsk images placed in disks subdirectory
as with the original utility



## LICENSING
All of this software is released under GPL license, much of it still untested so use at your own risk

released 10/22/2026 by
```
   ____      ____..--'   ____      ____..--'    .-''-.    .---.     
 .'  __ `.  |        | .'  __ `.  |        |  .'_ _   \   | ,_|     
/   '  \  \ |   .-'  '/   '  \  \ |   .-'  ' / ( ` )   ',-./  )     
|___|  /  | |.-'.'   /|___|  /  | |.-'.'   /. (_ o _)  |\  '_ '`)   
   _.-`   |    /   _/    _.-`   |    /   _/ |  (_,_)___| > (_)  )   
.'   _    |  .'._( )_ .'   _    |  .'._( )_ '  \   .---.(  .  .-'   
|  _( )_  |.'  (_'o._)|  _( )_  |.'  (_'o._) \  `-'    / `-'`-'|___ 
\ (_ o _) /|    (_,_)|\ (_ o _) /|    (_,_)|  \       /   |        \
 '.(_,_).' |_________| '.(_,_).' |_________|   `'-..-'    `--------`
 ```