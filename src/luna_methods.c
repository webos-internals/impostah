/*=============================================================================
 Copyright (C) 2010 WebOS Internals <support@webos-internals.org>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; either version 2
 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 =============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/stat.h>
#include <dirent.h>

#include "luna_service.h"
#include "luna_methods.h"

#define ALLOWED_CHARS "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-+_"

#define API_VERSION "2"

static char file_buffer[CHUNKSIZE+CHUNKSIZE+1];
static char file_esc_buffer[MAXBUFLEN];

static bool access_denied(LSMessage *message) {
  LSError lserror;
  LSErrorInit(&lserror);

  const char *appId = LSMessageGetApplicationID(message);
  if (!appId || strncmp(appId, "org.webosinternals.impostah", 27) || ((strlen(appId) > 27) && (*(appId+27) != ' '))) {
    if (!LSMessageRespond(message, "{\"returnValue\": false, \"errorText\": \"Unauthorised access\"}", &lserror)) {
      LSErrorPrint(&lserror, stderr);
      LSErrorFree(&lserror);
    }
    return true;
  }

  return false;
}


//
// Escape a string so that it can be used directly in a JSON response.
// In general, this means escaping quotes, backslashes and control chars.
// It uses the static esc_buffer, which must be twice as large as the
// largest string this routine can handle.
//
static char *json_escape_str(char *str, char *esc_buffer) {
  const char *json_hex_chars = "0123456789abcdef";

  // Initialise the output buffer
  strcpy(esc_buffer, "");

  // Check the constraints on the input string
  if (strlen(str) > MAXBUFLEN) return (char *)esc_buffer;

  // Initialise the pointers used to step through the input and output.
  char *resultsPt = (char *)esc_buffer;
  int pos = 0, start_offset = 0;

  // Traverse the input, copying to the output in the largest chunks
  // possible, escaping characters as we go.
  unsigned char c;
  do {
    c = str[pos];
    switch (c) {
    case '\0':
      // Terminate the copying
      break;
    case '\b':
    case '\n':
    case '\r':
    case '\t':
    case '"':
    case '\\': {
      // Copy the chunk before the character which must be escaped
      if (pos - start_offset > 0) {
	memcpy(resultsPt, str + start_offset, pos - start_offset);
	resultsPt += pos - start_offset;
      };
      
      // Escape the character
      if      (c == '\b') {memcpy(resultsPt, "\\b",  2); resultsPt += 2;} 
      else if (c == '\n') {memcpy(resultsPt, "\\n",  2); resultsPt += 2;} 
      else if (c == '\r') {memcpy(resultsPt, "\\r",  2); resultsPt += 2;} 
      else if (c == '\t') {memcpy(resultsPt, "\\t",  2); resultsPt += 2;} 
      else if (c == '"')  {memcpy(resultsPt, "\\\"", 2); resultsPt += 2;} 
      else if (c == '\\') {memcpy(resultsPt, "\\\\", 2); resultsPt += 2;} 

      // Reset the start of the next chunk
      start_offset = ++pos;
      break;
    }

    default:
      
      // Check for "special" characters
      if ((c < ' ') || (c > 127)) {

	// Copy the chunk before the character which must be escaped
	if (pos - start_offset > 0) {
	  memcpy(resultsPt, str + start_offset, pos - start_offset);
	  resultsPt += pos - start_offset;
	}

	// Insert a normalised representation
	sprintf(resultsPt, "\\u00%c%c",
		json_hex_chars[c >> 4],
		json_hex_chars[c & 0xf]);

	// Reset the start of the next chunk
	start_offset = ++pos;
      }
      else {
	// Just move along the source string, without copying
	pos++;
      }
    }
  } while (c);

  // Copy the final chunk, if required
  if (pos - start_offset > 0) {
    memcpy(resultsPt, str + start_offset, pos - start_offset);
    resultsPt += pos - start_offset;
  } 

  // Terminate the output buffer ...
  memcpy(resultsPt, "\0", 1);

  // and return a pointer to it.
  return (char *)esc_buffer;
}

//
// A dummy method, useful for unimplemented functions or as a status function.
// Called directly from webOS, and returns directly to webOS.
//
bool dummy_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  if (!LSMessageRespond(message, "{\"returnValue\": true}", &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Return the current API version of the service.
// Called directly from webOS, and returns directly to webOS.
//
bool version_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  if (!LSMessageRespond(message, "{\"returnValue\": true, \"version\": \"" VERSION "\", \"apiVersion\": \"" API_VERSION "\"}", &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run a shell command, and return the output in-line in a buffer for returning to webOS.
// The global run_command_buffer must be initialised before calling this function.
// The return value says whether the command executed successfully or not.
//
static bool run_command(char *command, bool escape, char *buffer) {
  LSError lserror;
  LSErrorInit(&lserror);

  char esc_buffer[MAXBUFLEN];

  // Local buffers to store the current and previous lines.
  char line[MAXLINLEN];

  // fprintf(stderr, "Running command %s\n", command);

  // buffer is assumed to be initialised, ready for strcat to append.

  // Is this the first line of output?
  bool first = true;

  bool array = false;

  // Start execution of the command, and read the output.
  FILE *fp = popen(command, "r");

  // Return immediately if we cannot even start the command.
  if (!fp) {
    return false;
  }

  // Loop through the output lines
  while (fgets(line, sizeof line, fp)) {

    // Chomp the newline
    char *nl = strchr(line,'\n'); if (nl) *nl = 0;

    // Add formatting breaks between lines
    if (first) {
      if (buffer[strlen(buffer)-1] == '[') {
	array = true;
      }
      first = false;
    }
    else {
      if (array) {
	strcat(buffer, ", ");
      }
      else {
	strcat(buffer, "<br>");
      }
    }
    
    // Append the unfiltered output to the buffer.
    if (escape) {
      if (array) {
	strcat(buffer, "\"");
      }
      strcat(buffer, json_escape_str(line, esc_buffer));
      if (array) {
	strcat(buffer, "\"");
      }
    }
    else {
      strcat(buffer, line);
    }
  }
  
  // Check the close status of the process
  if (pclose(fp)) {
    return false;
  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  // %%% We need a way to distinguish command failures from LSMessage failures %%%
  // %%% This may need to be true if we just want to ignore LSMessage failures %%%
  return false;
}

//
// Send a standard format command failure message back to webOS.
// The command will be escaped.  The output argument should be a JSON array and is not escaped.
// The additional text  will not be escaped.
// The return value is from the LSMessageRespond call, not related to the command execution.
//
static bool report_command_failure(LSMessage *message, char *command, char *stdErrText, char *additional) {
  LSError lserror;
  LSErrorInit(&lserror);

  char buffer[MAXBUFLEN];
  char esc_buffer[MAXBUFLEN];

  // Include the command that was executed, in escaped form.
  snprintf(buffer, MAXBUFLEN,
	   "{\"errorText\": \"Unable to run command: %s\"",
	   json_escape_str(command, esc_buffer));

  // Include any stderr fields from the command.
  if (stdErrText) {
    strcat(buffer, ", \"stdErr\": ");
    strcat(buffer, stdErrText);
  }

  // Report that an error occurred.
  strcat(buffer, ", \"returnValue\": false, \"errorCode\": -1");

  // Add any additional JSON fields.
  if (additional) {
    strcat(buffer, ", ");
    strcat(buffer, additional);
  }

  // Terminate the JSON reply message ...
  strcat(buffer, "}");

  // fprintf(stderr, "Message is %s\n", buffer);

  // and send it.
  if (!LSMessageRespond(message, buffer, &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Run a simple shell command, and return the output to webOS.
//
static bool simple_command(LSMessage *message, char *command) {
  LSError lserror;
  LSErrorInit(&lserror);

  char run_command_buffer[MAXBUFLEN];

  // Initialise the output buffer
  strcpy(run_command_buffer, "{\"stdOut\": [");

  // Run the command
  if (run_command(command, true, run_command_buffer)) {

    // Finalise the message ...
    strcat(run_command_buffer, "], \"returnValue\": true}");

    // fprintf(stderr, "Message is %s\n", run_command_buffer);

    // and send it to webOS.
    if (!LSMessageRespond(message, run_command_buffer, &lserror)) goto error;
  }
  else {

    // Finalise the command output ...
    strcat(run_command_buffer, "]");

    // and use it in a failure report message.
    if (!report_command_failure(message, command, run_command_buffer+11, NULL)) goto end;
  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

static bool read_file(LSMessage *message, char *filename, bool subscribed) {
  LSError lserror;
  LSErrorInit(&lserror);

  FILE * file = fopen(filename, "r");
  if (!file) {
    sprintf(file_buffer,
	    "{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Cannot open %s\"}",
	    filename);
    
    if (!LSMessageRespond(message, file_buffer, &lserror)) goto error;
    return true;
  }
  
  char chunk[CHUNKSIZE];
  int chunksize = CHUNKSIZE;

  syslog(LOG_DEBUG, "Reading file %s\n", filename);

  fseek(file, 0, SEEK_END);
  int filesize = ftell(file);
  fseek(file, 0, SEEK_SET);

  if (subscribed) {
    if (sprintf(file_buffer,
		"{\"returnValue\": true, \"filesize\": %d, \"chunksize\": %d, \"stage\": \"start\"}",
		filesize, chunksize)) {

      if (!LSMessageRespond(message, file_buffer, &lserror)) goto error;

    }
  }
  else if (filesize < chunksize) {
    chunksize = filesize;
  }

  int size;
  int datasize = 0;
  while ((size = fread(chunk, 1, chunksize, file)) > 0) {
    datasize += size;
    chunk[size] = '\0';
    sprintf(file_buffer, "{\"returnValue\": true, \"size\": %d, \"contents\": \"", size);
    strcat(file_buffer, json_escape_str(chunk, file_esc_buffer));
    strcat(file_buffer, "\"");
    if (subscribed) {
      strcat(file_buffer, ", \"stage\": \"middle\"");
    }
    strcat(file_buffer, "}");

    if (!LSMessageRespond(message, file_buffer, &lserror)) goto error;

  }

  if (!fclose(file)) {
    if (subscribed) {
      sprintf(file_buffer, "{\"returnValue\": true, \"datasize\": %d, \"stage\": \"end\"}", datasize);

      if (!LSMessageRespond(message, file_buffer, &lserror)) goto error;

    }
  }
  else {
    sprintf(file_buffer, "{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Cannot close file\"}");

    if (!LSMessageRespond(message, file_buffer, &lserror)) goto error;

  }

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool listKeys_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  // Local buffer to store the command
  char command[MAXLINLEN];

  sprintf(command, "sqlite3 /var/palm/data/keys.db 'SELECT id,ownerId,keyId FROM keytable ;' 2>&1");

  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool listBackups_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  // Local buffer to store the command
  char command[MAXLINLEN];

  sprintf(command, "/bin/ls -1 /etc/palm/backup/ 2>&1");

  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool getBackup_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  char filename[MAXLINLEN];

  // Extract the id argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *id = json_find_first_label(object, "id");               
  if (!id || (id->child->type != JSON_STRING) || (strspn(id->child->text, ALLOWED_CHARS) != strlen(id->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing id\"}",
			&lserror)) goto error;
    return true;
  }

  sprintf(filename, "/etc/palm/backup/%s", id->child->text);

  return read_file(message, filename, true);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool listFilecacheTypes_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  // Local buffer to store the command
  char command[MAXLINLEN];

  sprintf(command, "/bin/ls -1 /etc/palm/filecache_types/ 2>&1");

  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool getFilecacheType_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  char filename[MAXLINLEN];

  // Extract the id argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *type = json_find_first_label(object, "type");               
  if (!type || (type->child->type != JSON_STRING) || (strspn(type->child->text, ALLOWED_CHARS) != strlen(type->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing type\"}",
			&lserror)) goto error;
    return true;
  }

  sprintf(filename, "/etc/palm/filecache_types/%s", type->child->text);

  return read_file(message, filename, true);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool listConnections_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  if (access_denied(message)) return true;

  // Local buffer to store the command
  char command[MAXLINLEN];

  sprintf(command, "cat /proc/net/nf_conntrack 2>&1");

  return simple_command(message, command);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Dump the contents of an sqlite3 database table
//
static bool dump_sqlite(LSMessage *message, char *database, char *table) {
  LSError lserror;
  LSErrorInit(&lserror);

  char buffer[MAXBUFLEN];
  char esc_buffer[MAXBUFLEN];

  char line[MAXLINLEN];

  // Local buffer to store the command
  char command[MAXLINLEN];

  sprintf(command, "sqlite3 %s .dump 2>&1", database);

  // Is this the first line of output?
  bool first = true;

  // Was there an error in accessing any of the files?
  bool error = false;

  // Length of buffer before the last command
  int lastlen = 0;

  // Start execution of the command to list the config files.
  FILE *fp = popen(command, "r");

  // If the command cannot be started
  if (!fp) {

    // then report the error to webOS.
    if (!report_command_failure(message, command, NULL, NULL)) goto end;

    // The error report has been sent, so return to webOS.
    return true;
  }

  if (!LSMessageRespond(message, "{\"stage\": \"start\", \"returnValue\": true}", &lserror)) goto error;

  // Initialise the output message.
  strcpy(buffer, "{");
  lastlen = strlen(buffer);

  // Loop through the list of files in the scripts directory.
  while (fgets( line, sizeof line, fp)) {

    // Chomp the newline
    char *nl = strchr(line,'\n'); if (nl) *nl = 0;

    if ((strlen(line) <= 13+strlen(table)+9) ||
	strncmp(line, "INSERT INTO \"", 13) ||
	strncmp(line+13, table, strlen(table)) ||
	strncmp(line+13+strlen(table), "\" VALUES(", 9) ||
	strncmp(line+strlen(line)-2, ");", 2)) {
      continue;
    }

    *(line+strlen(line)-2) = 0;

    // Push out a partial chunk
    if (strlen(buffer) >= CHUNKSIZE) {

      // Terminate the JSON array
      if (!first) {
	strcat(buffer, "], ");
      }

      strcat(buffer, "\"stage\": \"middle\", ");

      // Check the error status, and return the current error status
      if (error) {
	strcat(buffer, "\"returnValue\": false}");
      }
      else {
	strcat(buffer, "\"returnValue\": true}");
      }
      
      // fprintf(stderr, "Message is %s\n", buffer);

      // Return the results to webOS.
      if (!LSMessageRespond(message, buffer, &lserror)) goto error;

      // This is now the first line of output
      first = true;

      // Initialise the output message.
      strcpy(buffer, "{");
      lastlen = strlen(buffer);
    }

    // Start or continue the JSON array
    if (first) {
      strcat(buffer, "\"results\": [");
      lastlen = strlen(buffer);
      first = false;
    }
    else if (strlen(buffer) > lastlen) {
      strcat(buffer, ", ");
      lastlen = strlen(buffer);
    }

    // Store the command output
    strcat(buffer, "\"");
    strcat(buffer, json_escape_str(line+13+strlen(table)+9, esc_buffer));
    strcat(buffer, "\"");
  }

  // Terminate the JSON array
  if (!first) {
    strcat(buffer, "], ");
  }

  strcat(buffer, "\"stage\": \"end\", ");

  // Check the close status of the process, and return the combined error status
  if (pclose(fp) || error) {
    strcat(buffer, "\"returnValue\": false}");
  }
  else {
    strcat(buffer, "\"returnValue\": true}");
  }

  // fprintf(stderr, "Message is %s\n", buffer);

  // Return the results to webOS.
  if (!LSMessageRespond(message, buffer, &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool listAppDatabases_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  struct stat statbuf;
  char *filename = "/home/root/html5-databases/Databases.db";

  if (stat(filename, &statbuf) == -1) {
    filename = "/var/palm/data/Databases.db";
  }

  return access_denied(message) || \
    dump_sqlite(message, filename, "Databases");
}

bool listAppCookies_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return access_denied(message) || \
    dump_sqlite(message, "/var/palm/data/cookies.db", "Cookies");
}

bool listWebCookies_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return access_denied(message) || \
    dump_sqlite(message, "/var/palm/data/browser-cookies.db", "Cookies");
}

bool listSystemPrefs_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return access_denied(message) || \
    dump_sqlite(message, "/var/luna/preferences/systemprefs.db", "Preferences");
}

//
// Handler for the impersonate service.
//
bool impersonate_handler(LSHandle* lshandle, LSMessage *reply, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessage* message = (LSMessage*)ctx;
  retVal = LSMessageRespond(message, LSMessageGetPayload(reply), &lserror);
  if (!LSMessageIsSubscription(message)) {
    LSMessageUnref(message);
  }
  if (!retVal) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }
  return retVal;
}

//
// Impersonate a call to the requested service and return the output to webOS.
//
bool impersonate_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessageRef(message);

  if (access_denied(message)) return true;

  // Extract the method argument from the message
  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *id = json_find_first_label(object, "id");               
  if (!id || (id->child->type != JSON_STRING)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing id\"}",
			&lserror)) goto error;
    return true;
  }

  // Extract the service argument from the message
  object = json_parse_document(LSMessageGetPayload(message));
  json_t *service = json_find_first_label(object, "service");               
  if (!service || (service->child->type != JSON_STRING)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing service\"}",
			&lserror)) goto error;
    return true;
  }

  // Extract the method argument from the message
  object = json_parse_document(LSMessageGetPayload(message));
  json_t *method = json_find_first_label(object, "method");               
  if (!method || (method->child->type != JSON_STRING)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing method\"}",
			&lserror)) goto error;
    return true;
  }

  // Extract the params argument from the message
  object = json_parse_document(LSMessageGetPayload(message));
  json_t *params = json_find_first_label(object, "params");               
  if (!params || (params->child->type != JSON_OBJECT)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing params\"}",
			&lserror)) goto error;
    return true;
  }

  char uri[MAXLINLEN];
  sprintf(uri, "palm://%s/%s", service->child->text, method->child->text);

  char *paramstring = NULL;
  json_tree_to_string (params->child, &paramstring);
  if (!LSCallFromApplication(priv_serviceHandle, uri, paramstring, id->child->text,
			     impersonate_handler, message, NULL, &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Get the listing of a directory, and return it's contents.
//
bool get_dir_listing_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  char buffer[MAXBUFLEN];
  char esc_buffer[MAXBUFLEN];

  struct dirent *ep;

  // Local buffer to hold each line of output from ls
  char line[MAXLINLEN];

  // Is this the first line of output?
  bool first = true;

  // Was there an error in accessing any of the files?
  bool error = false;

  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *id = json_find_first_label(object, "directory");

  if (!id || (id->child->type != JSON_STRING) || (strspn(id->child->text, ALLOWED_CHARS"/") != strlen(id->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Invalid or missing directory\"}",
			&lserror)) goto error;
  }

  // Start execution of the command to list the directory contents
  DIR *dp = opendir(id->child->text);

  // If the command cannot be started
  if (!dp) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, \"errorText\": \"Unable to open directory\"}",
			&lserror)) goto error;

    // The error report has been sent, so return to webOS.
    return true;
  }

  // Initialise the output message.
  strcpy(buffer, "{");

  // Loop through the list of directory entries.
  while (ep = readdir(dp)) {

    // Start or continue the JSON array
    if (first) {
      strcat(buffer, "\"contents\": [");
      first = false;
    }
    else {
      strcat(buffer, ", ");
    }

    strcat(buffer, "{\"name\":\"");
    strcat(buffer, json_escape_str(ep->d_name, esc_buffer));
    strcat(buffer, "\", ");

    strcat(buffer, "\"type\":\"");
    if (ep->d_type == DT_DIR) {
      strcat(buffer, "directory");
    }
    else if (ep->d_type == DT_REG) {
      strcat(buffer, "file");
    }
    else if (ep->d_type == DT_LNK) {
      strcat(buffer, "symlink");
    }
    else {
      strcat(buffer, "other");
    }
    strcat(buffer, "\"}");
  }

  // Terminate the JSON array
  if (!first) {
    strcat(buffer, "], ");
  }

  // Check the close status of the process, and return the combined error status
  if (closedir(dp) || error) {
    strcat(buffer, "\"returnValue\": false}");
  }
  else {
    strcat(buffer, "\"returnValue\": true}");
  }

  // Return the results to webOS.
  if (!LSMessageRespond(message, buffer, &lserror)) goto error;

  return true;
 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool get_file_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  char run_command_buffer[MAXBUFLEN];
  char command[MAXLINLEN];

  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *id;

  // Extract the filename argument from the message
  id = json_find_first_label(object, "filename");
  if (!id || (id->child->type != JSON_STRING) ||
      (strlen(id->child->text) >= MAXNAMLEN) ||
      (strspn(id->child->text, ALLOWED_CHARS) != strlen(id->child->text))) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, "
			"\"errorText\": \"Invalid or missing filename parameter\", "
			"\"stage\": \"failed\"}",
			&lserror)) goto error;
    return true;
  }
  char filename[MAXNAMLEN];
  sprintf(filename, "/media/internal/.temp/%s", id->child->text);

  // Extract the url argument from the message
  id = json_find_first_label(object, "url");               
  if (!id || (id->child->type != JSON_STRING) ||
      (strlen(id->child->text) >= MAXLINLEN)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, "
			"\"errorText\": \"Invalid or missing url parameter\", "
			"\"stage\": \"failed\"}",
			&lserror)) goto error;
    return true;
  }
  char url[MAXLINLEN];
  strcpy(url, id->child->text);

  if (!strncmp(url, "file://", 7)) {
    strcpy(filename, url+7);
  }
  else {

    /* Download the package */

    snprintf(command, MAXLINLEN,
	     "/usr/bin/curl --create-dirs --insecure --location --fail --show-error --output %s %s 2>&1", filename, url);

    strcpy(run_command_buffer, "{\"stdOut\": [");
    if (run_command(command, true, run_command_buffer)) {
      strcat(run_command_buffer, "], \"returnValue\": true, \"stage\": \"download\"}");
      if (!LSMessageRespond(message, run_command_buffer, &lserror)) goto error;
    }
    else {
      strcat(run_command_buffer, "]");
      if (!report_command_failure(message, command, run_command_buffer+11, "\"stage\": \"failed\"")) goto end;
      return true;
    }
  }

  return read_file(message, filename, true);

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

bool put_file_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  LSError lserror;
  LSErrorInit(&lserror);

  char buffer[MAXBUFLEN];

  json_t *object = json_parse_document(LSMessageGetPayload(message));
  json_t *id;

  // Extract the url argument from the message
  id = json_find_first_label(object, "filename");
  if (!id || (id->child->type != JSON_STRING) ||
      (strlen(id->child->text) >= MAXLINLEN) ||
      (strlen(id->child->text) < 8) ||
      strncmp(id->child->text, "file://", 7)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, "
			"\"errorText\": \"Invalid or missing filename parameter\"}",
			&lserror)) goto error;
    return true;
  }
  char filename[MAXLINLEN];
  strcpy(filename, id->child->text+7);

  // Extract the object argument from the message
  id = json_find_first_label(object, "object");
  if (!id || (id->child->type != JSON_OBJECT)) {
    if (!LSMessageRespond(message,
			"{\"returnValue\": false, \"errorCode\": -1, "
			"\"errorText\": \"Invalid or missing object parameter\"}",
			&lserror)) goto error;
    return true;
  }

  char *contents = NULL;

  if (json_tree_to_string(id->child, &contents) != JSON_OK) {
    sprintf(buffer,
	    "{\"errorText\": \"Unable to parse object\", \"returnValue\": false, \"errorCode\": -1 }");
    if (!LSMessageRespond(message, buffer, &lserror)) goto error;
    return true;
  }

  FILE *fp = fopen(filename, "w");
  if (!fp) {
    sprintf(buffer,
	    "{\"errorText\": \"Unable to open %s\", \"returnValue\": false, \"errorCode\": -1 }",
	    filename);
    if (!LSMessageRespond(message, buffer, &lserror)) goto error;
    return true;
  }

  if (fputs(contents, fp) == EOF) {
    (void)fclose(fp);
    (void)unlink(filename);
    sprintf(buffer,
	    "{\"errorText\": \"Unable to write to %s\", \"returnValue\": false, \"errorCode\": -1 }",
	    filename);
    if (!LSMessageRespond(message, buffer, &lserror)) goto error;
    return true;
  }
  
  if (fclose(fp)) {
    sprintf(buffer,
	    "{\"errorText\": \"Unable to close %s\", \"returnValue\": false, \"errorCode\": -1 }",
	    filename);
    if (!LSMessageRespond(message, buffer, &lserror)) goto error;
    return true;
  }

  if (!LSMessageRespond(message, "{\"returnValue\": true}", &lserror)) goto error;
  return true;

 error:
  LSErrorPrint(&lserror, stderr);
  LSErrorFree(&lserror);
 end:
  return false;
}

//
// Remove the ran-first-use flag file, and return the output to webOS.
//
bool remove_first_use_flag_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return simple_command(message, "/bin/rm -f /var/luna/preferences/ran-first-use 2>&1");
}

//
// Restart Luna, and return the output to webOS.
//
bool restart_luna_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return simple_command(message, "/usr/bin/killall -HUP LunaSysMgr 2>&1");
}

//
// Restart UpdateDaemon, and return the output to webOS.
//
bool restart_update_daemon_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  return simple_command(message, "/usr/bin/killall -HUP UpdateDaemon 2>&1");
}

//
// Handler for the addResourceHandler service.
//
bool addResource_handler(LSHandle* lshandle, LSMessage *reply, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessage* message = (LSMessage*)ctx;
  retVal = LSMessageRespond(message, LSMessageGetPayload(reply), &lserror);
  LSMessageUnref(message);
  if (!retVal) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }
  return retVal;
}

//
// Call the addResourceHandler service using liblunaservice and return the output to webOS.
//
bool addResource_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessageRef(message);
  const char *payload;
  payload = LSMessageGetPayload(message);
  retVal = LSCall(priv_serviceHandle, "palm://com.palm.applicationManager/addResourceHandler",
		  payload, addResource_handler, message, NULL, &lserror);
  if (!retVal) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }
  return retVal;
}

//
// Handler for the swapResourceHandler service.
//
bool swapResource_handler(LSHandle* lshandle, LSMessage *reply, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessage* message = (LSMessage*)ctx;
  retVal = LSMessageRespond(message, LSMessageGetPayload(reply), &lserror);
  LSMessageUnref(message);
  if (!retVal) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }
  return retVal;
}

//
// Call the swapResourceHandler service using liblunaservice and return the output to webOS.
//
bool swapResource_method(LSHandle* lshandle, LSMessage *message, void *ctx) {
  bool retVal;
  LSError lserror;
  LSErrorInit(&lserror);
  LSMessageRef(message);
  const char *payload;
  payload = LSMessageGetPayload(message);
  retVal = LSCall(priv_serviceHandle, "palm://com.palm.applicationManager/swapResourceHandler",
		  payload, swapResource_handler, message, NULL, &lserror);
  if (!retVal) {
    LSErrorPrint(&lserror, stderr);
    LSErrorFree(&lserror);
  }
  return retVal;
}

LSMethod luna_methods[] = {
  { "status",			dummy_method },
  { "version",			version_method },

  { "listKeys",			listKeys_method },

  { "listBackups",		listBackups_method },
  { "getBackup",		getBackup_method },

  { "listFilecacheTypes",	listFilecacheTypes_method },
  { "getFilecacheType",		getFilecacheType_method },

  { "listConnections",		listConnections_method },

  { "listAppDatabases",		listAppDatabases_method },
  { "listAppCookies",		listAppCookies_method },
  { "listWebCookies",		listWebCookies_method },

  { "listSystemPrefs",		listSystemPrefs_method },

  { "impersonate",		impersonate_method },

  { "getDirListing",		get_dir_listing_method },

  { "getFile",			get_file_method },
  { "putFile",			put_file_method },

  { "removeFirstUseFlag",	remove_first_use_flag_method },
  { "restartLuna",		restart_luna_method },
  { "restartUpdateDaemon",	restart_update_daemon_method },

  { "addResource",		addResource_method },
  { "swapResource",		swapResource_method },

  { 0, 0 }
};

bool register_methods(LSPalmService *serviceHandle, LSError lserror) {
  return LSPalmServiceRegisterCategory(serviceHandle, "/", luna_methods, luna_methods,
				       NULL, NULL, &lserror);
}
