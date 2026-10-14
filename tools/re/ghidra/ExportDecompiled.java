// Ghidra headless post-script for tools/re/decomp.py: make sure the entry
// points the project cares about are functions, then decompile every function
// in the program into one file each under the cache directory.
//
//   args: <out-dir> [<entry-points-file>]
//
// The entry-points file lists one address per line (`#` comments allowed):
// every host body hostregs.py finds and any callback a contributor names.
// Ghidra's auto-analysis misses code that is only ever reached through a
// pointer -- a host body is *pushed* to its registrar, a timer callback is
// stored in a table -- so those are disassembled and made functions here, as
// is any code address an instruction pushes or moves as an immediate.
//
// This reads the contributor's own gbr.exe on their machine. Its output is a
// local cache and must never be committed (docs/legal.md rule 2).

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.address.AddressSetView;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceManager;

import java.io.File;
import java.io.PrintWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Set;
import java.util.TreeSet;

public class ExportDecompiled extends GhidraScript {

    private FunctionManager functions;
    private Listing listing;

    /** Make `a` a function entry; true when one was created. */
    private boolean ensure(Address a) {
        if (functions.getFunctionAt(a) != null) return false;
        if (listing.getInstructionAt(a) == null) {
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            if (listing.getInstructionAt(a) == null) return false;
        }
        // An address inside a function that already exists is a label in its
        // body, not an entry: leave it to the function that owns it.
        if (functions.getFunctionContaining(a) != null) return false;
        return new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
    }

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        File out = new File(args[0]);
        File bodies = new File(out, "functions");
        bodies.mkdirs();
        functions = currentProgram.getFunctionManager();
        listing = currentProgram.getListing();

        int named = 0, pushed = 0;
        if (args.length > 1) {
            for (String line : Files.readAllLines(new File(args[1]).toPath())) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) continue;
                Address a = toAddr(Long.decode(line.split("\\s+")[0]));
                if (ensure(a)) named++;
            }
        }
        // Code addresses an instruction pushes or moves as an immediate and
        // that no function owns: callbacks handed to a registrar or a timer.
        AddressSetView code = currentProgram.getMemory().getExecuteSet();
        ReferenceManager refs = currentProgram.getReferenceManager();
        List<Address> candidates = new ArrayList<>();
        AddressIterator destinations = refs.getReferenceDestinationIterator(code, true);
        while (destinations.hasNext()) {
            Address a = destinations.next();
            if (functions.getFunctionContaining(a) != null) continue;
            for (Reference r : refs.getReferencesTo(a)) {
                Instruction from = listing.getInstructionAt(r.getFromAddress());
                if (from == null) continue;
                String m = from.getMnemonicString().toUpperCase();
                if (m.equals("PUSH") || m.equals("MOV")) {
                    candidates.add(a);
                    break;
                }
            }
        }
        for (Address a : candidates) if (ensure(a)) pushed++;
        println("created " + named + " named and " + pushed + " pushed entry points");

        DecompInterface decompiler = new DecompInterface();
        decompiler.setOptions(new DecompileOptions());
        decompiler.openProgram(currentProgram);
        int done = 0, failed = 0;
        try (PrintWriter index = new PrintWriter(new File(out, "index.tsv"), StandardCharsets.UTF_8)) {
            index.println("# entry\tname\tmin\tmax\tcallers\tcallees");
            for (Function f : functions.getFunctions(true)) {
                if (monitor.isCancelled()) break;
                if (f.isThunk() || f.isExternal()) continue;
                Set<String> callers = new TreeSet<>();
                for (Function c : f.getCallingFunctions(monitor)) callers.add(c.getEntryPoint().toString());
                Set<String> callees = new TreeSet<>();
                for (Function c : f.getCalledFunctions(monitor)) callees.add(c.getEntryPoint().toString());
                String entry = f.getEntryPoint().toString();
                index.println(entry + "\t" + f.getName() + "\t" + f.getBody().getMinAddress() + "\t"
                        + f.getBody().getMaxAddress() + "\t" + String.join(",", callers) + "\t"
                        + String.join(",", callees));
                DecompileResults r = decompiler.decompileFunction(f, 120, monitor);
                String text = r != null && r.decompileCompleted() && r.getDecompiledFunction() != null
                        ? r.getDecompiledFunction().getC()
                        : "/* decompile failed: " + (r == null ? "no result" : r.getErrorMessage()) + " */\n";
                if (r == null || !r.decompileCompleted()) failed++;
                Files.writeString(new File(bodies, entry + ".c").toPath(), text, StandardCharsets.UTF_8);
                if (++done % 1000 == 0) println("decompiled " + done);
            }
        }
        decompiler.dispose();
        println("decompiled " + done + " functions, " + failed + " failed");
    }
}
