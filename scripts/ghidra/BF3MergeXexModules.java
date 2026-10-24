// Merge XEX modules, each imported into its own program with XEXLoaderWV, into the current
// program, so one Ghidra program (and one GhidraMCP session) covers the whole game.
//
// Copies every memory block (bytes, permissions), every user/imported label, and every function
// entry point from each source program. A source block that collides with memory already in the
// current program goes into an overlay space named after its module instead (BF3: BF.Splash sits
// inside BF.Main's range), so that module keeps its real addresses as <space>::<addr>.
//
// GUI: open BF.Main, run the script, pick the other modules. Headless (-postScript): pass the
// project paths of the source programs as arguments, e.g. /Default.xex /Engine.BuildInfo.xex.
// Run auto-analysis afterwards; the script only creates functions at the copied entry points.
//@category BF3
//@author bf3-recomp

import java.io.ByteArrayInputStream;
import java.util.ArrayList;
import java.util.List;

import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.framework.model.DomainFile;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.address.AddressSpace;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Program;
import ghidra.program.model.mem.Memory;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Namespace;
import ghidra.program.model.symbol.SourceType;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolTable;
import ghidra.program.model.symbol.SymbolType;
import ghidra.util.exception.CancelledException;

public class BF3MergeXexModules extends GhidraScript {

	@Override
	protected void run() throws Exception {
		List<Program> sources = openSources();
		if (sources.isEmpty()) {
			println("No source programs selected.");
			return;
		}
		try {
			for (Program src : sources) {
				merge(src);
			}
		}
		finally {
			for (Program src : sources) {
				src.release(this);
			}
		}
	}

	private List<Program> openSources() throws Exception {
		List<Program> sources = new ArrayList<>();
		String[] args = getScriptArgs();
		if (args.length > 0) {
			for (String path : args) {
				DomainFile file = state.getProject().getProjectData().getFile(path);
				if (file == null) {
					throw new IllegalArgumentException("not in project: " + path);
				}
				sources.add((Program) file.getDomainObject(this, false, false, monitor));
			}
			return sources;
		}
		while (true) {
			try {
				Program p = askProgram("Module program to merge into " + currentProgram.getName());
				if (p == null || p == currentProgram) {
					break;
				}
				sources.add(p);
			}
			catch (CancelledException e) {
				break;
			}
			if (!askYesNo("Merge XEX modules", "Add another module?")) {
				break;
			}
		}
		return sources;
	}

	private void merge(Program src) throws Exception {
		if (!src.getLanguageID().equals(currentProgram.getLanguageID())) {
			throw new IllegalArgumentException(src.getName() + ": language " + src.getLanguageID() +
				" differs from " + currentProgram.getLanguageID());
		}
		String tag = src.getName().replaceAll("\\.xex$", "").replaceAll("[^A-Za-z0-9_]", "_");
		Memory mem = currentProgram.getMemory();
		AddressSpace defaultSpace = currentProgram.getAddressFactory().getDefaultAddressSpace();

		// Decide once per module: everything goes in the default space, or all of it in an
		// overlay, so references between the module's own sections keep resolving together.
		boolean collides = false;
		for (MemoryBlock b : src.getMemory().getBlocks()) {
			Address start = defaultSpace.getAddress(b.getStart().getOffset());
			if (mem.intersects(start, start.add(b.getSize() - 1))) {
				collides = true;
			}
		}
		// An overlay space with no blocks yet maps getAddress() to its base space, so the copy below
		// uses getAddressInThisSpaceOnly() throughout.
		AddressSpace space =
			collides ? currentProgram.createOverlaySpace(tag, defaultSpace) : defaultSpace;
		println(String.format("%s -> %s", src.getName(),
			collides ? "overlay space '" + space.getName() + "'" : "default space"));

		for (MemoryBlock b : src.getMemory().getBlocks()) {
			monitor.checkCancelled();
			if (!b.getStart().getAddressSpace().isLoadedMemorySpace()) {
				continue;
			}
			Address start = space.getAddressInThisSpaceOnly(b.getStart().getOffset());
			String name = tag + "_" + b.getName();
			MemoryBlock nb;
			if (b.isInitialized()) {
				byte[] bytes = new byte[(int) b.getSize()];
				b.getBytes(b.getStart(), bytes);
				nb = mem.createInitializedBlock(name, start, new ByteArrayInputStream(bytes),
					bytes.length, monitor, false);
			}
			else {
				nb = mem.createUninitializedBlock(name, start, b.getSize(), false);
			}
			nb.setPermissions(b.isRead(), b.isWrite(), b.isExecute());
			nb.setVolatile(b.isVolatile());
			nb.setComment(src.getName() + " " + b.getName());
			println(String.format("  %-24s %s-%s %s%s%s", name, nb.getStart(), nb.getEnd(),
				b.isRead() ? "r" : "-", b.isWrite() ? "w" : "-", b.isExecute() ? "x" : "-"));
		}

		SymbolTable dstSymbols = currentProgram.getSymbolTable();
		Namespace global = currentProgram.getGlobalNamespace();
		int labels = 0;
		for (Symbol s : src.getSymbolTable().getAllSymbols(false)) {
			monitor.checkCancelled();
			SymbolType type = s.getSymbolType();
			if (s.getSource() == SourceType.DEFAULT || !s.getAddress().isMemoryAddress() ||
				(type != SymbolType.LABEL && type != SymbolType.FUNCTION)) {
				continue;
			}
			Address a = space.getAddressInThisSpaceOnly(s.getAddress().getOffset());
			if (!mem.contains(a)) {
				continue;
			}
			dstSymbols.createLabel(a, s.getName(), global, SourceType.IMPORTED);
			labels++;
		}

		AddressSet entries = new AddressSet();
		for (Function f : src.getFunctionManager().getFunctions(true)) {
			Address a = space.getAddressInThisSpaceOnly(f.getEntryPoint().getOffset());
			if (mem.contains(a)) {
				entries.add(a);
			}
		}
		new DisassembleCommand(entries, null, true).applyTo(currentProgram, monitor);
		new CreateFunctionCmd(entries, SourceType.IMPORTED).applyTo(currentProgram, monitor);
		println(String.format("  %d labels, %d function entries", labels, entries.getNumAddresses()));
	}
}
