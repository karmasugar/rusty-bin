// Export the listing and the function table of the current program to text files for offline analysis.
// Writes <dir>/<program>.lst (one line per instruction) and <dir>/<program>.functions.tsv.
// Offsets are relative to the file image start (after the MZ header for EXE files).
// Args: <output directory>
// @category Rusty

import java.io.File;
import java.io.PrintWriter;
import java.util.Set;
import java.util.TreeSet;

import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.SegmentedAddress;
import ghidra.program.model.listing.CodeUnit;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;

public class RustyExport extends GhidraScript {

	private long imageStart;

	private String offset(Address address) {
		return String.format("%06x", address.getOffset() - imageStart);
	}

	private String segmented(Address address) {
		if (address instanceof SegmentedAddress) {
			SegmentedAddress segmented = (SegmentedAddress) address;
			return String.format("%04x:%04x", segmented.getSegment(), segmented.getSegmentOffset());
		}
		return address.toString();
	}

	@Override
	protected void run() throws Exception {
		String[] args = getScriptArgs();
		File directory = new File(args.length > 0 ? args[0] : "export");
		directory.mkdirs();
		imageStart = Long.MAX_VALUE;
		for (MemoryBlock block : currentProgram.getMemory().getBlocks()) {
			if (block.getStart() instanceof SegmentedAddress && block.isInitialized()) {
				imageStart = Math.min(imageStart, block.getStart().getOffset());
			}
		}
		String name = currentProgram.getName();

		try (PrintWriter out = new PrintWriter(new File(directory, name + ".lst"), "UTF-8")) {
			InstructionIterator instructions = currentProgram.getListing().getInstructions(true);
			while (instructions.hasNext() && !monitor.isCancelled()) {
				Instruction instruction = instructions.next();
				Address address = instruction.getAddress();
				Function function = getFunctionAt(address);
				if (function != null) {
					out.println();
					out.println(String.format("; ==== FUNCTION %s %s (%s)", offset(address), function.getName(),
						segmented(address)));
				}
				StringBuilder bytes = new StringBuilder();
				for (byte value : instruction.getBytes()) {
					bytes.append(String.format("%02x", value & 0xFF));
				}
				String comment = instruction.getComment(CodeUnit.EOL_COMMENT);
				out.println(String.format("%s %s %-14s %s%s", offset(address), segmented(address), bytes, instruction,
					comment == null ? "" : "   ; " + comment.replace('\n', ' ')));
			}
		}

		try (PrintWriter out = new PrintWriter(new File(directory, name + ".functions.tsv"), "UTF-8")) {
			out.println("offset\tsegmented\tname\tinstructions\tcallers\tcallees\tcalled_by\tcalls");
			FunctionIterator functions = currentProgram.getFunctionManager().getFunctions(true);
			while (functions.hasNext()) {
				Function function = functions.next();
				int count = 0;
				InstructionIterator body = currentProgram.getListing().getInstructions(function.getBody(), true);
				while (body.hasNext()) {
					body.next();
					count++;
				}
				Set<String> callers = new TreeSet<>();
				for (Function caller : function.getCallingFunctions(monitor)) {
					callers.add(offset(caller.getEntryPoint()));
				}
				Set<String> callees = new TreeSet<>();
				for (Function callee : function.getCalledFunctions(monitor)) {
					callees.add(offset(callee.getEntryPoint()));
				}
				out.println(String.join("\t", offset(function.getEntryPoint()), segmented(function.getEntryPoint()),
					function.getName(), String.valueOf(count), String.valueOf(callers.size()),
					String.valueOf(callees.size()), String.join(",", callers), String.join(",", callees)));
			}
		}
		println(name + ": exported to " + directory);
	}
}
