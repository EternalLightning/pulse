$ErrorActionPreference = 'Stop'
$source = 'E:\Projects\pulse\bench_data\audit-medium-ee159e3460384dcea2ee343d8aa2ca3e\pulse_user_storage_test.exe'
$task_root = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ('Temp\PulseStorageMedium-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($task_root) | Out-Null
$exe = Join-Path $task_root 'pulse_user_storage_test.exe'
[IO.File]::WriteAllBytes($exe, [IO.File]::ReadAllBytes($source))
$task_temp = Join-Path $task_root 'fixture-temp'
[IO.Directory]::CreateDirectory($task_temp) | Out-Null
$env:TEMP = $task_temp
$env:TMP = $task_temp
$log = Join-Path $task_root 'default-test.log'
$code = @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class StorageMediumProbe {
 [StructLayout(LayoutKind.Sequential)] public struct SA { public int Length; public IntPtr Descriptor; [MarshalAs(UnmanagedType.Bool)] public bool Inherit; }
 [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)] public struct SI {
  public int cb; public string reserved, desktop, title; public uint x,y,xs,ys,xc,yc,fill,flags; public short show,reserved2; public IntPtr reservedp,input,output,error;
 }
 [StructLayout(LayoutKind.Sequential)] public struct PI { public IntPtr process,thread; public uint pid,tid; }
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern bool CreateProcessW(string app, StringBuilder command, IntPtr pa,IntPtr ta,bool inherit,uint flags,IntPtr env,string cwd,ref SI si,out PI pi);
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateFileW(string path,uint access,uint share,ref SA sa,uint disposition,uint flags,IntPtr template);
 [DllImport("kernel32.dll", SetLastError=true)] static extern uint ResumeThread(IntPtr thread);
 [DllImport("kernel32.dll")] static extern uint WaitForSingleObject(IntPtr process,uint timeout);
 [DllImport("kernel32.dll")] static extern bool GetExitCodeProcess(IntPtr process,out uint code);
 [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
 [DllImport("kernel32.dll")] static extern bool TerminateProcess(IntPtr process,uint code);
 [DllImport("advapi32.dll", SetLastError=true)] static extern bool OpenProcessToken(IntPtr process,uint access,out IntPtr token);
 [DllImport("advapi32.dll", SetLastError=true)] static extern bool GetTokenInformation(IntPtr token,int info,IntPtr buffer,int length,out int returned);
 [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
 [DllImport("advapi32.dll")] static extern IntPtr GetSidSubAuthority(IntPtr sid,uint index);
 public static string Run(string exe,string cwd,string log) {
  var sa=new SA { Length=Marshal.SizeOf(typeof(SA)),Inherit=true };
  IntPtr output=CreateFileW(log,0x40000000,1,ref sa,2,0x80,IntPtr.Zero);
  IntPtr input=CreateFileW("NUL",0x80000000,3,ref sa,3,0x80,IntPtr.Zero);
  if(output==new IntPtr(-1) || input==new IntPtr(-1)) throw new Exception("log/input error="+Marshal.GetLastWin32Error());
  var si=new SI {cb=Marshal.SizeOf(typeof(SI)),flags=0x100,input=input,output=output,error=output}; PI pi;
  if(!CreateProcessW(exe,new StringBuilder("\""+exe+"\""),IntPtr.Zero,IntPtr.Zero,true,0x08000004,IntPtr.Zero,cwd,ref si,out pi)) throw new Exception("CreateProcess error="+Marshal.GetLastWin32Error());
  try {
   IntPtr token;
   if(!OpenProcessToken(pi.process,8,out token)) throw new Exception("OpenToken error="+Marshal.GetLastWin32Error());
   int bytes; GetTokenInformation(token,25,IntPtr.Zero,0,out bytes); IntPtr data=Marshal.AllocHGlobal(bytes); int rid;
   try {
    if(!GetTokenInformation(token,25,data,bytes,out bytes)) throw new Exception("GetToken error="+Marshal.GetLastWin32Error());
    IntPtr sid=Marshal.ReadIntPtr(data); byte count=Marshal.ReadByte(GetSidSubAuthorityCount(sid));
    rid=Marshal.ReadInt32(GetSidSubAuthority(sid,(uint)(count-1)));
   } finally {Marshal.FreeHGlobal(data); CloseHandle(token);}
   if(ResumeThread(pi.thread)==0xffffffff) throw new Exception("Resume failed");
   if(WaitForSingleObject(pi.process,30000)!=0) {TerminateProcess(pi.process,0xdead); throw new Exception("owned fixture timed out");}
   uint exit; GetExitCodeProcess(pi.process,out exit);
   return "child_pid="+pi.pid+" child_integrity_rid="+rid+" exit_code="+exit+" exit_hex=0x"+exit.ToString("X8");
  } finally {CloseHandle(pi.thread);CloseHandle(pi.process);CloseHandle(output);CloseHandle(input);}
 }
}
"@
Add-Type -TypeDefinition $code
$source_hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
$target_hash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
if($source_hash -ne $target_hash) { throw 'Content hash mismatch' }
Write-Output ('source_sha256=' + $source_hash)
Write-Output ('copy_sha256=' + $target_hash)
Write-Output ('copy=' + $exe)
Write-Output ('log=' + $log)
& icacls.exe $exe
Write-Output ([StorageMediumProbe]::Run($exe,$task_root,$log))
Get-Content -LiteralPath $log
