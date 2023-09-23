# stealer

This is a simple Windows application meant to be installed on a removable drive and started from a terminal.
It copies all files that match filter settings (size and file extension).
Multiple drives and users are supported. A basic log file is created for later inspection.

# options

_________________________________________________________________________________________________________
| Name  | Long name    | Description                                   | Default                        |
|-------|--------------|-----------------------------------------------|--------------------------------|
| -s    | --size       | Maximum file size in KB                       | 5000 (5 MB)                    |
| -e    | --extensions | Accepted file extensions, separated by commas | .txt, .docx, .pptx, .pdf, .csv |
| -t    | --target     | Full destination path                         | D:\\Data                       |
| -q    | --quiet      | Silences console output                       | off                            |

# example
`.\stealer.exe --size 1000 --extensions .pdf,.txt,.csv --target E:\stuff`
