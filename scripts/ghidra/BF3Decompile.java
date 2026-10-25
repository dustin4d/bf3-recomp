// Decompile functions to a text file. Read-only.
// Arguments: <out.txt> <range-or-address>...
//   range:   <start>-<end>   every function whose entry lies in [start, end)
//   address: <addr>          the function containing addr
// Addresses may carry an overlay prefix (BF_Splash::83970000).
//@category BF3
//@author bf3-recomp

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.LinkedHashSet;
import java.util.Set;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;

public class BF3Decompile extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length < 2) {
			throw new IllegalArgumentException("usage: BF3Decompile <out.txt> <range-or-address>...");
		}
		Set<Function> functions = new LinkedHashSet<>();
		for (int i = 1; i < args.length; i++) {
			String arg = args[i];
			int dash = arg.indexOf('-');
			if (dash > 0) {
				Address start = toAddr(arg.substring(0, dash));
				Address end = toAddr(arg.substring(dash + 1));
				for (Function f : currentProgram.getFunctionManager()
						.getFunctions(new AddressSet(start, end.subtract(1)), true)) {
					functions.add(f);
				}
			}
			else {
				Function f = getFunctionContaining(toAddr(arg));
				if (f == null) {
					println("NOFUNC " + arg);
				}
				else {
					functions.add(f);
				}
			}
		}

		DecompInterface decomp = new DecompInterface();
		decomp.openProgram(currentProgram);
		int ok = 0;
		try (PrintWriter out = new PrintWriter(Files.newBufferedWriter(Path.of(args[0])))) {
			for (Function f : functions) {
				monitor.checkCancelled();
				DecompileResults r = decomp.decompileFunction(f, 60, monitor);
				out.printf("// ===== %s @ %s =====%n", f.getName(), f.getEntryPoint().toString(true));
				if (r.decompileCompleted()) {
					out.println(r.getDecompiledFunction().getC());
					ok++;
				}
				else {
					out.println("// decompile failed: " + r.getErrorMessage());
				}
			}
		}
		finally {
			decomp.dispose();
		}
		println(String.format("DECOMPILED %d/%d functions to %s", ok, functions.size(), args[0]));
	}
}
