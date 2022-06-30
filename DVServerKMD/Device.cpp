/*++

Module Name:

    device.c - Device handling events for example driver.

Abstract:

   This file contains the device entry points and callbacks.
    
Environment:

    Kernel-mode Driver Framework

--*/

#include "driver.h"
#include "debug.h"
#include "baseobj.h"
#include "viogpulite.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text (PAGE, DVServerKMDCreateDevice)
#endif

NTSTATUS
DVServerKMDCreateDevice(
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
/*++

Routine Description:

    Worker routine called to create a device and its software resources.

Arguments:

    DeviceInit - Pointer to an opaque init structure. Memory for this
                    structure will be freed by the framework when the WdfDeviceCreate
                    succeeds. So don't access the structure after that point.

Return Value:

    NTSTATUS

--*/
{
    WDF_OBJECT_ATTRIBUTES deviceAttributes;
    PDEVICE_CONTEXT pDeviceContext;
    WDFDEVICE device;
    NTSTATUS status;

    PAGED_CODE();
    TRACING();
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &device);

    if (NT_SUCCESS(status)) {
        //
        // Get a pointer to the device context structure that we just associated
        // with the device object. We define this structure in the device.h
        // header file. DeviceGetContext is an inline function generated by
        // using the WDF_DECLARE_CONTEXT_TYPE_WITH_NAME macro in device.h.
        // This function will do the type checking and return the device context.
        // If you pass a wrong object handle it will return NULL and assert if
        // run under framework verifier mode.
        //
        pDeviceContext = DeviceGetContext(device);

        //
        // Initialize the context.
        //
        pDeviceContext->PrivateDeviceData = 0;

        //
        // Create the DVServerKMD device instance
        //
        pDeviceContext->pvDeviceExtension = (PVOID) new(NonPagedPoolNx) VioGpuAdapterLite(pDeviceContext);

        if (!pDeviceContext->pvDeviceExtension)
            return STATUS_UNSUCCESSFUL;

        //
        // Create a device interface so that applications can find and talk
        // to us.
        //
        status = WdfDeviceCreateDeviceInterface(
            device,
            &GUID_DEVINTERFACE_DVServerKMD,
            NULL // ReferenceString
            );

        if (NT_SUCCESS(status)) {
            //
            // Initialize the I/O Package and any Queues
            //
            status = DVServerKMDQueueInitialize(device);
        }

        if (NT_SUCCESS(status)) {
            //
            // Create the interrupt object
            //
            WDF_INTERRUPT_CONFIG interruptConfig;

            WDF_INTERRUPT_CONFIG_INIT(&interruptConfig,
                DVServerKMDEvtInterruptISR,
                DVServerKMDEvtInterruptDPC);

            //
            // These first two callbacks will be called at DIRQL. Their job is to
            // enable and disable interrupts.
            //
            interruptConfig.EvtInterruptEnable = DVServerKMDEvtInterruptEnable;
            interruptConfig.EvtInterruptDisable = DVServerKMDEvtInterruptDisable;

            status = WdfInterruptCreate(device,
                &interruptConfig,
                WDF_NO_OBJECT_ATTRIBUTES,
                &pDeviceContext->WdfInterrupt);

            if (!NT_SUCCESS(status)) {
                ERR("WdfInterruptCreate() call failed!\n");
                return STATUS_UNSUCCESSFUL;
            }
        }
    }

    return status;
}

NTSTATUS DVServerKMDEvtPrepareHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated)
/*++

Routine Description:

    Performs whatever initialization is needed to setup the device, setting up
    a DMA channel or mapping any I/O port resources.  This will only be called
    as a device starts or restarts, not every time the device moves into the D0
    state.  Consequently, most hardware initialization belongs elsewhere.

Arguments:

    Device - A handle to the WDFDEVICE

    Resources - The raw PnP resources associated with the device.  Most of the
    time, these aren't useful for a PCI device.

    ResourcesTranslated - The translated PnP resources associated with the
    device.  This is what is important to a PCI device.

Return Value:

    NT status code - failure will result in the device stack being torn down

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT pDeviceContext;
    VioGpuAdapterLite* pVioGpuAdapterLite;
    TRACING();
    pDeviceContext = DeviceGetContext(Device);
    pDeviceContext->WdfDevice = Device;
    pDeviceContext->ResourcesRaw = ResourcesRaw;

    //
    // Get the BUS_INTERFACE_STANDARD for our device so that we can
    // read & write to PCI config space.
    //
    status = WdfFdoQueryForInterface(Device,
        &GUID_BUS_INTERFACE_STANDARD,
        (PINTERFACE)&pDeviceContext->BusInterface,
        sizeof(BUS_INTERFACE_STANDARD),
        1,
        NULL);

    if (!NT_SUCCESS(status))
        return status;

    //
    // Retrieve the DVServerKMD device instance
    //
    pVioGpuAdapterLite = (VioGpuAdapterLite*)pDeviceContext->pvDeviceExtension;

    if (!pVioGpuAdapterLite)
        return STATUS_UNSUCCESSFUL;

    //
    // TODO:
    //  The following display info will be passed down from DVServerUMD
    //  For now, just use hard coded one
    //
    DXGK_DISPLAY_INFORMATION DisplayInfo = {
        1024,
        768,
        4096,
        D3DDDIFMT_X8R8G8B8,
        0,
        0,
        0
    };

    status = pVioGpuAdapterLite->HWInit(ResourcesTranslated, &DisplayInfo);

    return status;
}

NTSTATUS DVServerKMDEvtReleaseHardware(
    _In_ WDFDEVICE Device,
    _In_ WDFCMRESLIST ResourcesTranslated)
/*++

Routine Description:

    Unmap the resources that were mapped in PLxEvtDevicePrepareHardware.
    This will only be called when the device stopped for resource rebalance,
    surprise-removed or query-removed.

Arguments:

    Device - A handle to the WDFDEVICE

    ResourcesTranslated - The translated PnP resources associated with the
        device.  This is what is important to a PCI device.

Return Value:

    NT status code - failure will result in the device stack being torn down

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    TRACING();
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    return status;
}

NTSTATUS DVServerKMDEvtD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
/*++

Routine Description:

    This routine prepares the device for use.  It is called whenever the device
    enters the D0 state, which happens when the device is started, when it is
    restarted, and when it has been powered off.

    Note that interrupts will not be enabled at the time that this is called.
    They will be enabled after this callback completes.

    This function is not marked pageable because this function is in the
    device power up path. When a function is marked pagable and the code
    section is paged out, it will generate a page fault which could impact
    the fast resume behavior because the client driver will have to wait
    until the system drivers can service this page fault.

Arguments:

    Device  - The handle to the WDF device object

    PreviousState - The state the device was in before this callback was invoked.

Return Value:

    NTSTATUS

    Success implies that the device can be used.

    Failure will result in the    device stack being torn down.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT pDeviceContext;
    VioGpuAdapterLite* pVioGpuAdapterLite;
    TRACING();
    UNREFERENCED_PARAMETER(PreviousState);

    pDeviceContext = DeviceGetContext(Device);

    //
    // Retrieve the DVServerKMD device instance
    //
    pVioGpuAdapterLite = (VioGpuAdapterLite*)pDeviceContext->pvDeviceExtension;

    if (!pVioGpuAdapterLite)
        return STATUS_UNSUCCESSFUL;

    status = pVioGpuAdapterLite->SetPowerState(PowerDeviceD0);

    return status;
}

NTSTATUS DVServerKMDEvtD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState)
/*++

Routine Description:

    This routine undoes anything done in PLxEvtDeviceD0Entry.  It is called
    whenever the device leaves the D0 state, which happens when the device
    is stopped, when it is removed, and when it is powered off.

    The device is still in D0 when this callback is invoked, which means that
    the driver can still touch hardware in this routine.

    Note that interrupts have already been disabled by the time that this
    callback is invoked.

Arguments:

    Device  - The handle to the WDF device object

    TargetState - The state the device will go to when this callback completes.

Return Value:

    Success implies that the device can be used.  Failure will result in the
    device stack being torn down.

--*/
{
    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_CONTEXT pDeviceContext;
    VioGpuAdapterLite* pVioGpuAdapterLite;

    UNREFERENCED_PARAMETER(PreviousState);

    pDeviceContext = DeviceGetContext(Device);

    //
    // Retrieve the DVServerKMD device instance
    //
    pVioGpuAdapterLite = (VioGpuAdapterLite*)pDeviceContext->pvDeviceExtension;

    if (!pVioGpuAdapterLite)
        return STATUS_UNSUCCESSFUL;

    status = pVioGpuAdapterLite->SetPowerState(PowerDeviceD3);

    return status;
}

BOOLEAN DVServerKMDEvtInterruptISR(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG MessageID)
/*++

    Routine Description:

    Interrupt handler for this driver. Called at DIRQL level when the
    device or another device sharing the same interrupt line asserts
    the interrupt. The driver first checks the device to make sure whether
    this interrupt is generated by its device and if so clear the interrupt
    register to disable further generation of interrupts and queue a
    DPC to do other I/O work related to interrupt - such as reading
    the device memory, starting a DMA transaction, coping it to
    the request buffer and completing the request, etc.

Arguments:

    Interupt   - Handle to WDFINTERRUPT Object for this device.
    MessageID  - MSI message ID (always 0 in this configuration)

Return Value:

    TRUE   --  This device generated the interrupt.
    FALSE  --  This device did not generated this interrupt.

--*/
{
    PDEVICE_CONTEXT pDeviceContext;
    VioGpuAdapterLite* pVioGpuAdapterLite;

    pDeviceContext = DeviceGetContext(WdfInterruptGetDevice(Interrupt));

    //
    // Retrieve the DVServerKMD device instance
    //
    pVioGpuAdapterLite = (VioGpuAdapterLite*)pDeviceContext->pvDeviceExtension;

    if (!pVioGpuAdapterLite)
        return FALSE;

    pVioGpuAdapterLite->InterruptRoutine(MessageID);

    return TRUE;
}

void DVServerKMDEvtInterruptDPC(
    _In_ WDFINTERRUPT Interrupt,
    _In_ WDFOBJECT AssociatedObject)
/*++

    Routine Description:

Routine Description:

    DPC callback for ISR. Please note that on a multiprocessor system,
    you could have more than one DPCs running simulataneously on
    multiple processors. So if you are accesing any global resources
    make sure to synchrnonize the accesses with a spinlock.

Arguments:

    Interupt  - Handle to WDFINTERRUPT Object for this device.
    Device    - WDFDEVICE object passed to InterruptCreate

Return Value:

--*/
{
    PDEVICE_CONTEXT pDeviceContext;
    VioGpuAdapterLite* pVioGpuAdapterLite;

    UNREFERENCED_PARAMETER(AssociatedObject);

    pDeviceContext = DeviceGetContext(WdfInterruptGetDevice(Interrupt));

    //
    // Retrieve the DVServerKMD device instance
    //
    pVioGpuAdapterLite = (VioGpuAdapterLite*)pDeviceContext->pvDeviceExtension;

    if (!pVioGpuAdapterLite)
        return;

    pVioGpuAdapterLite->DpcRoutine();
}

NTSTATUS DVServerKMDEvtInterruptEnable(
    IN WDFINTERRUPT  Interrupt,
    IN WDFDEVICE     AssociatedDevice
)
/*++

Routine Description:

    This event is called when the Framework moves the device to D0, and after
    EvtDeviceD0Entry.  The driver should enable its interrupt here.

    This function will be called at the device's assigned interrupt
    IRQL (DIRQL.)

Arguments:

    Interrupt - Handle to a Framework interrupt object.

    AssociatedDevice - Handle to a Framework device object.

Return Value:

    BOOLEAN - TRUE indicates that the interrupt was successfully enabled.

--*/
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(AssociatedDevice);

    return STATUS_SUCCESS;
}

NTSTATUS DVServerKMDEvtInterruptDisable(
    IN WDFINTERRUPT  Interrupt,
    IN WDFDEVICE     AssociatedDevice
)
/*++

Routine Description:

    This event is called before the Framework moves the device to D1, D2 or D3
    and before EvtDeviceD0Exit.  The driver should disable its interrupt here.

    This function will be called at the device's assigned interrupt
    IRQL (DIRQL.)

Arguments:

    Interrupt - Handle to a Framework interrupt object.

    AssociatedDevice - Handle to a Framework device object.

Return Value:

    STATUS_SUCCESS -  indicates success.

--*/
{
    UNREFERENCED_PARAMETER(Interrupt);
    UNREFERENCED_PARAMETER(AssociatedDevice);

    return STATUS_SUCCESS;
}

