#include <ntifs.h>
#include <intrin.h>
#include "ia32.hpp"

extern "C"
{
	void gp_handler_stub( );
}

struct reg_context
{
	UINT64 rax;
	UINT64 rcx;
	UINT64 rdx;
	UINT64 rbx;
	UINT64 rbp;
	UINT64 rsi;
	UINT64 rdi;
	UINT64 r8;
	UINT64 r9;
	UINT64 r10;
	UINT64 r11;
	UINT64 r12;
	UINT64 r13;
	UINT64 r14;
	UINT64 r15;
};

#define MSR_MISC_FEATURES_ENABLES 0x140

segment_descriptor_interrupt_gate_64* gp_entries[ 256 ];
extern "C" UINT64 original_gp_handler = 0;
ULONG cpu_count = 0;
bool first = true;

bool check_cpu( )
{
	int regs[ 4 ]{};
	__cpuid( regs, 0 );

	if ( regs[ 1 ] != 0x756E6547 ||    // "Genu"
			regs[ 3 ] != 0x49656E69 || // "ineI"
			regs[ 2 ] != 0x6C65746E )  // "ntel"
		return false;
	

	__cpuid( regs, 1 );

	UINT32 eax = ( UINT32 )regs[ 0 ];

	UINT32 family = ( eax >> 8 ) & 0xF;
	UINT32 model = ( eax >> 4 ) & 0xF;
	UINT32 ext_model = ( eax >> 16 ) & 0xF;

	UINT32 display_model = model | (ext_model << 4);

	return ( family == 0x6 && display_model >= 0x3A ); // >=Ivy Bridge
}

UINT64 read_idt_entry( segment_descriptor_interrupt_gate_64* entry )
{
	return ( UINT64 )entry->offset_low |
		( ( UINT64 )entry->offset_middle << 16 ) |
		( ( UINT64 )entry->offset_high << 32 );
}

void write_idt_entry( segment_descriptor_interrupt_gate_64* entry, UINT64 addr )
{
	entry->offset_low = ( UINT16 )( addr & 0xFFFF );
	entry->offset_middle = ( UINT16 )( ( addr >> 16 ) & 0xFFFF );
	entry->offset_high = ( UINT32 )( ( addr >> 32 ) & 0xFFFFFFFF );
}

extern "C" void gp_handler( reg_context* context, UINT64 faulting_rip )
{
	UNREFERENCED_PARAMETER( faulting_rip );

	UINT32 leaf = ( UINT32 )context->rax;
	UINT32 subleaf = ( UINT32 )context->rcx;

	int regs[ 4 ];
	__cpuidex( regs, leaf, subleaf );

	switch ( leaf )
	{
		case 0:
		{
			regs[ 1 ] = 0x6B616552; // "Reak"
			regs[ 3 ] = 0x6B616552;	// "Reak"
			regs[ 2 ] = 0x6B616552;	// "Reak"
			break;
		}
		case 0x40000000:
		{
			regs[ 0 ] = 0x0;
			regs[ 1 ] = 0x76486F4E; // "NoHv"
			regs[ 2 ] = 0x76486F4E; // "NoHv"
			regs[ 3 ] = 0x76486F4E; // "NoHv"
			break;
		}
		default:
		{
			break;
		}
	}

	context->rax = ( UINT64 )regs[ 0 ];
	context->rbx = ( UINT64 )regs[ 1 ];
	context->rcx = ( UINT64 )regs[ 2 ];
	context->rdx = ( UINT64 )regs[ 3 ];

	UINT64* saved_rip = ( UINT64* )( ( UINT8* )context + sizeof( reg_context ) + 0x8 ); // [original rsp + 8]
	*saved_rip += 2;
}

ULONG_PTR hook_gp_handler( ULONG_PTR context )
{
	UNREFERENCED_PARAMETER( context );

	ULONG cpu_index = KeGetCurrentProcessorIndex( );

	write_idt_entry( gp_entries[ cpu_index ], ( UINT64 )gp_handler_stub ); // writing idt entry to our custom handler

	UINT64 msr = __readmsr( MSR_MISC_FEATURES_ENABLES ); // enabling cpuid faulting for CPL > 0
	msr |= 1ULL;
	__writemsr( MSR_MISC_FEATURES_ENABLES, msr );

	return 0;
}

ULONG_PTR restore_gp_handler( ULONG_PTR context )
{
	UNREFERENCED_PARAMETER( context );

	ULONG cpu_index = KeGetCurrentProcessorIndex( );

	write_idt_entry( gp_entries[ cpu_index ], ( UINT64 )original_gp_handler );

	UINT64 msr = __readmsr( MSR_MISC_FEATURES_ENABLES );
	msr &= ~1ULL;
	__writemsr( MSR_MISC_FEATURES_ENABLES, msr );

	return 0;
}

bool init( )
{
	cpu_count = KeQueryActiveProcessorCount( nullptr );
	for ( SIZE_T i = 0; i < cpu_count; ++i )
	{
		KAFFINITY original = KeSetSystemAffinityThreadEx( 1ULL << i );

		segment_descriptor_register_64 idtr;
		__sidt( &idtr ); // load idt of each processor

		segment_descriptor_interrupt_gate_64* gp_entry = ( segment_descriptor_interrupt_gate_64* )idtr.base_address + general_protection; // get #GP entry at IDT[13]

		if ( first )
		{
			original_gp_handler = read_idt_entry( gp_entry ); // #GP entry points to KiGeneralProtectionFault in all cpus
			first = false;
		}

		PHYSICAL_ADDRESS phys = MmGetPhysicalAddress( gp_entry );
		if ( !phys.QuadPart )
		{
			for ( SIZE_T j = 0; j < i; ++j )
			{
				if ( gp_entries[ j ] )
				{
					MmUnmapIoSpace( gp_entries[ j ], sizeof( segment_descriptor_interrupt_gate_64 ) );
					gp_entries[ j ] = nullptr;
				}
			}
			KeRevertToUserAffinityThreadEx( original );
			return false;
		}

		// remap a read only page to write on it
		gp_entries[ i ] = ( segment_descriptor_interrupt_gate_64* )MmMapIoSpaceEx( phys, sizeof( segment_descriptor_interrupt_gate_64 ), PAGE_READWRITE );
		if ( !gp_entries[ i ] )
		{
			for ( SIZE_T j = 0; j < i; ++j )
			{
				if ( gp_entries[ j ] )
				{
					MmUnmapIoSpace( gp_entries[ j ], sizeof( segment_descriptor_interrupt_gate_64 ) );
					gp_entries[ j ] = nullptr;
				}
			}
			KeRevertToUserAffinityThreadEx( original );
			return false;
		}

		KeRevertToUserAffinityThreadEx( original );
	}

	KeIpiGenericCall( hook_gp_handler, 0 ); // hook #GP handler and enable cpuid faulting for all CPUs
	return true;
}

void unload( PDRIVER_OBJECT driver_object )
{
	UNREFERENCED_PARAMETER( driver_object );

	DbgPrint( "unloading.\n" );

	KeIpiGenericCall( restore_gp_handler, 0 );

	for ( SIZE_T i = 0; i < cpu_count; ++i )
	{
		if ( gp_entries[ i ] )
		{
			MmUnmapIoSpace( gp_entries[ i ], sizeof( segment_descriptor_interrupt_gate_64 ) );
			gp_entries[ i ] = nullptr;
		}
	}
}

NTSTATUS driver_entry( PDRIVER_OBJECT driver_object, PUNICODE_STRING registry_path )
{
	UNREFERENCED_PARAMETER( registry_path );

	driver_object->DriverUnload = unload;

	DbgPrint( "CpuidFaultNoHv entry.\n" );

	if ( !check_cpu( ) )
	{
		DbgPrint( "cpu is not supported.\n" );
		return STATUS_NOT_SUPPORTED;
	}

	if ( !init( ) )
	{
		DbgPrint( "failed to initialize.\n" );
		return STATUS_UNSUCCESSFUL;
	}

	return STATUS_SUCCESS;
}