30-SEP-2026

Started working on the Time-Travel Debugger Phase 01 project.
Implemented the Stack class.
Implemented the stack constructor and methods: push(), pop(), peek(), isEmpty(), depth(), and snapshot_into().
Saved and pushed the updated server.cpp to GitHub.

3-OCT-2026

Completed the Timeline doubly linked list.
Implemented Pass 0x0 validation for function boundaries and nested functions.
Implemented readSourceLine(), firstWord(), and secondWord() helper functions.
Implemented writeResolveRecord() and readResolveRecord().
Implemented Pass 0x1 resolveProgram().
Added function tracking using FuncEntry.
Added pending CALL patch tracking using PendingPatch.
Verified that resolve.bin is generated correctly.
Verified that CALL targets are patched to the correct function offsets.
Tested the Resolve stage successfully on Linux/WSL using g++.

7-OCT-2026

I continued the project from Pass 0x2 (Execution).
What I did:
I compiled server.cpp on Ubuntu with g++ (-Wall -Wextra).
I made a small test program in source.bin.
I ran ./Server. It passed validation and resolve,
   ran the program, and made 7 snapshots.
 The file session.tdbg was created.

How I tested:
- I checked that session.tdbg starts with "TTDB" (the file ID).
- I used a small Python script to read the header,
  all snapshots, and the index. It printed VERIFY OK,
  so the file format is correct.
- I also did a simple check with xxd and grep.
  It printed PASS, so the value of a is 8 in the last step.

Bug found:
When a function ended, the changed values were not
copied back to the right variable in the caller.
In the test, the last step showed a = 5. It should be a = 8.

Reason:
The code saved the data at one stack position,
but read it from a different position (off by one).

Fix:
I changed this line in func_end:
  finishedDepth = callStack.depth();
to:
  finishedDepth = callStack.depth() - 1;

After the fix:
I ran the test again. The last step now shows a = 8.
The bug is fixed.
