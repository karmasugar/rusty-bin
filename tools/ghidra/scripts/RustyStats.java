// Print memory blocks, entry points, function and instruction counts of the current program.
// @category Rusty

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressIterator;
import ghidra.program.model.mem.MemoryBlock;

public class RustyStats extends GhidraScript {

	@Override
	protected void run() throws Exception {
		println("program " + currentProgram.getName() + " language " + currentProgram.getLanguageID());
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			println(String.format("  block %-12s %s - %s  size %d  init %b", block.getName(), block.getStart(),
				block.getEnd(), block.getSize(), block.isInitialized()));
		}
		AddressIterator entries = currentProgram.getSymbolTable().getExternalEntryPointIterator();
		while (entries.hasNext()) {
			Address entry = entries.next();
			println("  entry " + entry);
		}
		int functions = currentProgram.getFunctionManager().getFunctionCount();
		long instructions = currentProgram.getListing().getNumInstructions();
		println(String.format("  functions %d  instructions %d", functions, instructions));
	}
}
