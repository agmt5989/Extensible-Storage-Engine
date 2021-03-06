// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "esefile.hxx"



static const INT cbWrite        = 1024 * 1024;


static const INT cbRead         = 64 * 1024;


static const INT cblocks        = 16;


static const INT cbPageAlignment        = 4096;

struct COPYFILECONTROL
{
    BOOL            fIgnoreDiskErrors;

    QWORD           ibOffset;
    QWORD           cbSize;

    QWORD           cbTotalRead;
    QWORD           cbTotalWritten;
    LONG            cReadErrors;
    QWORD           cbReadErrors;
    LONG            cWriteErrors;
    QWORD           cbWriteErrors;

    JET_ERR         errCopyFileError;
};

VOID PrintCopyFileStatistics( COPYFILECONTROL * const pcopyfilecontrol )
{
    wprintf( L"\r\n" );
    wprintf(    L"\tTotal bytes read                = %#I64x (%I64d) (%I64d MB)\r\n",
                pcopyfilecontrol->cbTotalRead,
                pcopyfilecontrol->cbTotalRead,
                pcopyfilecontrol->cbTotalRead / 1024 / 1024 );
    wprintf(    L"\tTotal bytes written             = %#I64x (%I64d) (%I64d MB)\r\n",
                pcopyfilecontrol->cbTotalWritten,
                pcopyfilecontrol->cbTotalWritten,
                pcopyfilecontrol->cbTotalWritten / 1024 / 1024 );

    if ( pcopyfilecontrol->fIgnoreDiskErrors )
    {
        wprintf(    L"\tNumber of read errors           = %d\r\n",
                    pcopyfilecontrol->cReadErrors );
        wprintf(    L"\tBytes affected by read errors   = %#I64x (%I64d) (%I64d kB)\r\n",
                    pcopyfilecontrol->cbReadErrors, 
                    pcopyfilecontrol->cbReadErrors, 
                    pcopyfilecontrol->cbReadErrors / 1024 );
        wprintf(    L"\tNumber of write errors          = %d\r\n",
                    pcopyfilecontrol->cWriteErrors );
        wprintf(    L"\tBytes affected by write errors  = %#I64x (%I64d) (%I64d kB)\r\n",
                    pcopyfilecontrol->cbWriteErrors, 
                    pcopyfilecontrol->cbWriteErrors, 
                    pcopyfilecontrol->cbWriteErrors / 1024 );
    }
}

class CCopyContext
{
    public:

        CCopyContext(   COPYFILECONTROL* const  pcopyfilecontrolIn,
                        CSemaphore* const       psemIn,
                        IFileAPI* const         pfapiSrcIn,
                        IFileAPI* const         pfapiDestIn,
                        BYTE* const             pbDataIn )
            :   pcopyfilecontrol( pcopyfilecontrolIn ),
                psem( psemIn ),
                pfapiSrc( pfapiSrcIn ),
                pfapiDest( pfapiDestIn ),
                pbData( pbDataIn )
        {
        }

        ~CCopyContext()
        {
            OSMemoryPageFree( pbData );
            psem->Release();
        }

    public:

        COPYFILECONTROL* const  pcopyfilecontrol;
        CSemaphore* const       psem;
        IFileAPI* const         pfapiSrc;
        IFileAPI* const         pfapiDest;
        BYTE* const             pbData;
        QWORD                   ibOffset;
        DWORD                   cbData;
};


static void CollectStatistics(
    CCopyContext * const    pcopycontext,
    const ERR               err,
    const BOOL              fWrite )
{
    COPYFILECONTROL* const  pcopyfilecontrol    = pcopycontext->pcopyfilecontrol;

    if ( !fWrite )
    {
        AtomicAdd( &pcopyfilecontrol->cbTotalRead, pcopycontext->cbData );
        if ( err < JET_errSuccess )
        {
            AtomicIncrement( &pcopyfilecontrol->cReadErrors );
            AtomicAdd( &pcopyfilecontrol->cbReadErrors, pcopycontext->cbData );
        }
    }
    else
    {
        AtomicAdd( &pcopyfilecontrol->cbTotalWritten, pcopycontext->cbData );
        if ( err < JET_errSuccess )
        {
            AtomicIncrement( &pcopyfilecontrol->cWriteErrors );
            AtomicAdd( &pcopyfilecontrol->cbWriteErrors, pcopycontext->cbData );
        }
    }

    if ( err < JET_errSuccess )
    {
        WCHAR wszError[ 512 ] = { 0 };

        (void)JetGetSystemParameterW(   JET_instanceNil,
                                        JET_sesidNil,
                                        JET_paramErrorToString,
                                        (ULONG_PTR *)&err,
                                        wszError,
                                        sizeof( wszError ) );
        wprintf(    L"%s at offset 0x%016llx for %d bytes failed with error %d (%s).\n",
                    fWrite ? L"Write" : L"Read",
                    pcopycontext->ibOffset,
                    pcopycontext->cbData,
                    err,
                    wszError );

        AtomicCompareExchange( &pcopyfilecontrol->errCopyFileError, JET_errSuccess, err );
    }
}

static ERR ErrIssueNextRead( CCopyContext* const pcopycontext );

static VOID IOHandoff(  const ERR               errIO,
                        IFileAPI* const         pfapiInner,
                        const FullTraceContext& tc,
                        const OSFILEQOS         grbitQOS,
                        const QWORD             ibOffset,
                        const DWORD             cbData,
                        const BYTE* const       pbData,
                        CCopyContext* const     pcopycontext,
                        void* const             pvIOContext )
{
    ERR err = JET_errSuccess;

    Call( errIO );

HandleError:
    if ( err < JET_errSuccess )
    {
        delete pcopycontext;
    }
}

static VOID WriteComplete(  const ERR               errIO,
                            IFileAPI* const         pfapi,
                            const FullTraceContext& tc,
                            const OSFILEQOS         grbitQOS,
                            const QWORD             ibOffset,
                            const DWORD             cbData,
                            const BYTE* const       pbData,
                            CCopyContext* const     pcopycontext )
{
    ERR err = JET_errSuccess;

    CollectStatistics( pcopycontext, errIO, fTrue );
    if ( !pcopycontext->pcopyfilecontrol->fIgnoreDiskErrors )
    {
        Call( errIO );
    }

    Call( ErrIssueNextRead( pcopycontext ) );
    CallS( pcopycontext->pfapiSrc->ErrIOIssue() );

HandleError:
    if ( err < JET_errSuccess )
    {
        delete pcopycontext;
    }
}

static VOID ReadComplete(   ERR                     errIO,
                            IFileAPI* const         pfapi,
                            const FullTraceContext& tc,
                            const OSFILEQOS         grbitQOS,
                            const QWORD             ibOffset,
                            const DWORD             cbData,
                            const BYTE* const       pbData,
                            CCopyContext* const     pcopycontext )
{
    ERR                 err = JET_errSuccess;
    TraceContextScope   tcScope( iorpDirectAccessUtil );


    if ( errIO == JET_errFileIOBeyondEOF )
    {
        errIO = JET_errSuccess;
    }

    CollectStatistics( pcopycontext, errIO, fFalse );
    if ( !pcopycontext->pcopyfilecontrol->fIgnoreDiskErrors )
    {
        Call( errIO );
    }


    if ( errIO >= JET_errSuccess )
    {
        Call( pcopycontext->pfapiDest->ErrIOWrite(  *tcScope,
                                                    ibOffset,
                                                    cbData,
                                                    pbData,
                                                    qosIONormal,
                                                    (IFileAPI::PfnIOComplete)WriteComplete,
                                                    (DWORD_PTR)pcopycontext,
                                                    (IFileAPI::PfnIOHandoff)IOHandoff ) );
        CallS( pcopycontext->pfapiDest->ErrIOIssue() );
    }
    else
    {
        Call( ErrIssueNextRead( pcopycontext ) );
        CallS( pcopycontext->pfapiSrc->ErrIOIssue() );
    }

HandleError:
    if ( err < JET_errSuccess )
    {
        delete pcopycontext;
    }
}

static ERR ErrIssueNextRead( CCopyContext* const pcopycontext )
{
    ERR                 err     = JET_errSuccess;
    TraceContextScope   tcScope( iorpDirectAccessUtil );

    const QWORD ibOffset = AtomicAdd( &pcopycontext->pcopyfilecontrol->ibOffset, cbWrite ) - cbWrite;

    if ( ibOffset >= pcopycontext->pcopyfilecontrol->cbSize )
    {
        Call( ErrERRCheck( JET_errFileIOBeyondEOF ) );
    }

    pcopycontext->ibOffset = ibOffset;
    pcopycontext->cbData = (DWORD)min( cbWrite, roundup( pcopycontext->pcopyfilecontrol->cbSize - ibOffset, cbPageAlignment ) );

    Call( pcopycontext->pfapiSrc->ErrIORead(    *tcScope,
                                                pcopycontext->ibOffset,
                                                pcopycontext->cbData,
                                                pcopycontext->pbData,
                                                qosIONormal,
                                                (IFileAPI::PfnIOComplete)ReadComplete,
                                                (DWORD_PTR)pcopycontext,
                                                (IFileAPI::PfnIOHandoff)IOHandoff ) );

HandleError:
    return err;
}


JET_ERR ErrCopyFile(
    const wchar_t * const szFileSrc,
    const wchar_t * const szFileDest,
    BOOL fIgnoreDiskErrors )
{
    ERR                 err             = JET_errSuccess;

    IFileSystemAPI*     pfsapi          = NULL;
    IFileAPI*           pfapiSrc        = NULL;
    IFileAPI*           pfapiDest       = NULL;
    BYTE*               pbBlock         = NULL;
    CCopyContext*       pcopycontext    = NULL;

    COPYFILECONTROL     copyfilecontrol = { 0 };
    CSemaphore          sem( CSyncBasicInfo( "FChecksumFile" ) );
    TraceContextScope   tcScope( iorpDirectAccessUtil );

    INT                 iblockio        = 0;

    copyfilecontrol.fIgnoreDiskErrors = fIgnoreDiskErrors;


    wprintf( L"     Source File: %.64ls", ( iswascii( szFileSrc[0] ) ? szFileSrc : L"<unprintable>" ) );
    wprintf( L"\r\n" );
    wprintf( L"Destination File: %.64ls", ( iswascii( szFileDest[0] ) ? szFileDest : L"<unprintable>" ) );
    wprintf( L"\r\n\r\n" );
    InitStatus( L"Copy Progress (% complete)" );


    class CFileSystemConfiguration : public CDefaultFileSystemConfiguration
    {
        public:

            CFileSystemConfiguration()
            {
                m_dtickAccessDeniedRetryPeriod = 0;
                m_cbMaxReadSize = cbRead;
                m_cbMaxWriteSize = cbWrite;
                m_fBlockCacheEnabled = fTrue;
            }
    } fsconfig;

    Call( ErrOSFSCreate( &fsconfig, &pfsapi ) );


    Call( pfsapi->ErrFileOpen( szFileSrc, IFileAPI::fmfReadOnlyPermissive, &pfapiSrc ) );


    Call( pfapiSrc->ErrSize( &copyfilecontrol.cbSize, IFileAPI::filesizeLogical ) );


    Call( pfsapi->ErrFileCreate( szFileDest, IFileAPI::fmfNone, &pfapiDest ) );


    Call( pfapiDest->ErrSetSize( *tcScope, roundup( copyfilecontrol.cbSize, cbPageAlignment ), fFalse, qosIONormal ) );


    for ( iblockio = 0; iblockio < cblocks; ++iblockio )
    {
        Alloc( pbBlock = (BYTE*)PvOSMemoryPageAlloc( cbWrite, NULL ) );
        Alloc( pcopycontext = new CCopyContext( &copyfilecontrol, &sem, pfapiSrc, pfapiDest, pbBlock ) );
        pbBlock = NULL;

        err = ErrIssueNextRead( pcopycontext );
        if ( err == JET_errFileIOBeyondEOF )
        {
            delete pcopycontext;
            err = JET_errSuccess;
        }
        Call( err );
        pcopycontext = NULL;
    }

    Call( pfapiSrc->ErrIOIssue() );


    while ( iblockio > 0 )
    {
        const BOOL fAcquired = sem.FAcquire( 100 );
        if ( !fAcquired )
        {
            const INT  iPercentage = (INT)( ( copyfilecontrol.cbTotalWritten * 100 ) / copyfilecontrol.cbSize );
            UpdateStatus( iPercentage );
        }
        else
        {
            iblockio--;
        }
    }


    Call( pfapiDest->ErrSetSize( *tcScope, copyfilecontrol.cbSize, fFalse, qosIONormal ) );

    TermStatus();

    PrintCopyFileStatistics( &copyfilecontrol );

HandleError:
    OSMemoryPageFree( pbBlock );
    delete pcopycontext;
    if ( pfapiSrc )
    {
        (VOID)pfapiSrc->ErrIOIssue();
    }
    if ( pfapiDest )
    {
        (VOID)pfapiDest->ErrIOIssue();
    }
    for ( ; iblockio > 0; iblockio-- )
    {
        sem.Acquire();
    }
    delete pfapiDest;
    delete pfapiSrc;
    delete pfsapi;

    if ( copyfilecontrol.errCopyFileError < JET_errSuccess )
    {
        err = copyfilecontrol.errCopyFileError;
        if ( copyfilecontrol.fIgnoreDiskErrors )
        {
            err = ErrERRCheck( JET_wrnDatabaseRepaired );
        }
    }
    return err;
}


