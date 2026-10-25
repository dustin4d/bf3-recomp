// Write every function of the current program as "<address> <size> <name>" lines (address with its
// space prefix for overlays) to the file given as the only argument. Read-only.
//@category BF3
//@author bf3-recomp

import java.io.PrintWriter;
import java.nio.file.Files;
import java.nio.file.Path;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;

public class BF3ExportFunctions extends GhidraScript {

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		if (args.length != 1) {
			throw new IllegalArgumentException("usage: BF3ExportFunctions <out.txt>");
		}
		int count = 0;
		try (PrintWriter out = new PrintWriter(Files.newBufferedWriter(Path.of(args[0])))) {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				out.printf("%s %d %s%n", f.getEntryPoint().toString(true), f.getBody().getNumAddresses(),
					f.getName());
				count++;
			}
		}
		println(String.format("EXPORT %d functions to %s", count, args[0]));
	}
}
