import { useState, useEffect, useRef } from 'react';
import { marked } from 'marked';
import * as Y from 'yjs';
import { yCollab, yUndoManagerKeymap } from 'y-codemirror.next';
import { EditorView, basicSetup } from 'codemirror';
import { EditorState } from '@codemirror/state';
import { keymap } from '@codemirror/view';
import { markdown } from '@codemirror/lang-markdown';
import * as syncProtocol from 'y-protocols/sync';
import * as awarenessProtocol from 'y-protocols/awareness';
import { Awareness } from 'y-protocols/awareness';
import * as encoding from 'lib0/encoding';
import * as decoding from 'lib0/decoding';

const MESSAGE_SYNC = 0;
const MESSAGE_AWARENESS = 1;

const USER_COLORS = [
  { color: '#d97706', light: '#fef3c7' }, // Amber
  { color: '#2563eb', light: '#dbeafe' }, // Blue
  { color: '#059669', light: '#d1fae5' }, // Emerald
  { color: '#7c3aed', light: '#ede9fe' }, // Purple
  { color: '#dc2626', light: '#fee2e2' }, // Crimson
  { color: '#0d9488', light: '#ccfbf1' }, // Teal
];

function getUserColor(str) {
  if (!str) return USER_COLORS[0];
  let hash = 0;
  for (let i = 0; i < str.length; i++) {
    hash = str.charCodeAt(i) + ((hash << 5) - hash);
  }
  const index = Math.abs(hash) % USER_COLORS.length;
  return USER_COLORS[index];
}

export default function NoteEditor({ note, token, userEmail, onSaveNote, onRefreshNote, onShareNote }) {
  // Keep these states
  const [activeTab, setActiveTab] = useState('edit');
  const [isSaving, setIsSaving] = useState(false);
  const [saveError, setSaveError] = useState(false);
  const [uploading, setUploading] = useState(false);
  const [deletingId, setDeletingId] = useState(null);
  const [showAttachments, setShowAttachments] = useState(false);
  const [showShareModal, setShowShareModal] = useState(false);
  const [shareEmail, setShareEmail] = useState('');
  const [sharePermission, setSharePermission] = useState('editor');
  const [sharedUsers, setSharedUsers] = useState([]);
  const [sharing, setSharing] = useState(false);
  const [shareStatus, setShareStatus] = useState(null);
  const [exportingPdf, setExportingPdf] = useState(false);
  const [pdfExportResult, setPdfExportResult] = useState(null);
  const [wsConnected, setWsConnected] = useState(false);
  const [collaborators, setCollaborators] = useState([]);
  const [gqlData, setGqlData] = useState(null);
  const [gqlLoading, setGqlLoading] = useState(false);
  const [newCommentText, setNewCommentText] = useState('');
  const [submittingComment, setSubmittingComment] = useState(false);

  const downloadPdfFile = (base64Data, filename) => {
    try {
      const byteCharacters = atob(base64Data);
      const byteNumbers = new Array(byteCharacters.length);
      for (let i = 0; i < byteCharacters.length; i++) {
        byteNumbers[i] = byteCharacters.charCodeAt(i);
      }
      const byteArray = new Uint8Array(byteNumbers);
      const blob = new Blob([byteArray], { type: 'application/pdf' });
      const url = window.URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      document.body.removeChild(a);
      window.URL.revokeObjectURL(url);
    } catch (e) {
      console.error('Error triggering file download:', e);
    }
  };

  const handleExportPdf = async () => {
    if (!note?.id) return;
    setExportingPdf(true);
    setPdfExportResult(null);
    try {
      const res = await fetch(`/api/notes/${note.id}/export-pdf`, {
        method: 'POST',
        headers: {
          'Authorization': `Bearer ${token}`,
        },
      });
      const data = await res.json();
      if (res.ok && data.status === 'completed') {
        const filename = `${(note.title || 'Note').replace(/[^a-z0-9_-]/gi, '_')}.pdf`;
        if (data.pdf_base64) {
          downloadPdfFile(data.pdf_base64, filename);
        }
        setPdfExportResult({ ...data, filename });
      } else {
        alert('PDF Export failed: ' + (data.error || 'Unknown error'));
      }
    } catch (err) {
      console.error('PDF Export error:', err);
      alert('Failed to export PDF: ' + err.message);
    } finally {
      setExportingPdf(false);
    }
  };

  const fetchGraphQLDetails = async () => {
    if (!note?.id) return;
    setGqlLoading(true);
    try {
      const query = `
        query GetNoteDetails($id: ID!) {
          note(id: $id) {
            id
            title
            createdAt
            author {
              id
              email
            }
            comments {
              id
              content
              createdAt
              author {
                id
                email
              }
            }
          }
        }
      `;
      const res = await fetch('/api/graphql', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({ query, variables: { id: note.id } }),
      });
      const result = await res.json();
      if (result.data && result.data.note) {
        setGqlData(result.data.note);
      }
    } catch (err) {
      console.error('GraphQL query error:', err);
    } finally {
      setGqlLoading(false);
    }
  };

  useEffect(() => {
    setGqlData(null);
    if (activeTab === 'comments' && note?.id) {
      fetchGraphQLDetails();
    }
  }, [note?.id, activeTab]);

  const handleAddComment = async (e) => {
    e.preventDefault();
    if (!newCommentText.trim() || !note?.id) return;
    setSubmittingComment(true);
    try {
      const mutation = `
        mutation AddComment($noteId: ID!, $content: String!) {
          createComment(noteId: $noteId, content: $content) {
            id
            content
            createdAt
            author {
              id
              email
            }
          }
        }
      `;
      const res = await fetch('/api/graphql', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({ query: mutation, variables: { noteId: note.id, content: newCommentText } }),
      });
      const result = await res.json();
      if (result.data && result.data.createComment) {
        setNewCommentText('');
        fetchGraphQLDetails();
      }
    } catch (err) {
      console.error('GraphQL comment mutation error:', err);
    } finally {
      setSubmittingComment(false);
    }
  };

  const effectiveEmail = userEmail || localStorage.getItem('notenest_email') || '';
  const displayName = effectiveEmail ? (effectiveEmail.includes('@') ? effectiveEmail.split('@')[0] : effectiveEmail) : 'User';
  const userColor = getUserColor(effectiveEmail || displayName);
  
  // NEW: Yjs refs
  const ydocRef = useRef(null);
  const awarenessRef = useRef(null);
  const editorViewRef = useRef(null);
  const editorContainerRef = useRef(null);
  const wsRef = useRef(null);
  const ytextRef = useRef(null);
  const ytitleRef = useRef(null);
  
  // Title synced from Yjs for React rendering
  const [title, setTitle] = useState('');
  // Content string for preview tab
  const [previewContent, setPreviewContent] = useState('');
  const [docVersion, setDocVersion] = useState(0);
  
  const isViewer = note && note.permission === 'viewer';

  // Compute save status by comparing Yjs content against persisted note prop
  const saveStatus = isSaving ? 'Saving...' :
    saveError ? 'Error saving' :
    (note && ytextRef.current && ytitleRef.current && 
     (ytitleRef.current.toString() !== (note.title || '') || 
      ytextRef.current.toString() !== (note.content || ''))) ? 'Unsaved changes' :
    'All changes saved';

  const fetchShares = async () => {
    if (!note || isViewer) return;
    try {
      const res = await fetch(`/api/notes/${note.id}/shares`, {
        headers: { Authorization: `Bearer ${token}` },
      });
      if (res.ok) {
        const data = await res.json();
        setSharedUsers(data);
      }
    } catch (err) {
      console.error('Failed to fetch shares:', err);
    }
  };
  
  useEffect(() => {
    if (showShareModal) {
      fetchShares();
    }
  }, [showShareModal]);
  
  // Main Yjs + WebSocket + CodeMirror setup effect
  useEffect(() => {
    if (!note || !token) return;
    
    const ydoc = new Y.Doc();
    const awareness = new Awareness(ydoc);
    const ytext = ydoc.getText('content');
    const ytitle = ydoc.getText('title');
    
    ydocRef.current = ydoc;
    awarenessRef.current = awareness;
    ytextRef.current = ytext;
    ytitleRef.current = ytitle;
    
    let ws = null;
    let reconnectTimer = null;
    let isMounted = true;
    let initTimer = null;
    
    // Observe title changes from Yjs and sync to React state
    const titleObserver = () => {
      if (isMounted) setTitle(ytitle.toString());
    };
    ytitle.observe(titleObserver);
    
    const updateHandler = (update, origin) => {
      if (isMounted) setDocVersion(v => v + 1);
      if (origin !== 'remote' && ws && ws.readyState === WebSocket.OPEN) {
        const encoder = encoding.createEncoder();
        encoding.writeVarUint(encoder, MESSAGE_SYNC);
        syncProtocol.writeUpdate(encoder, update);
        ws.send(encoding.toUint8Array(encoder));
      }
    };
    ydoc.on('update', updateHandler);
    
    const awarenessUpdateHandler = ({ added, updated, removed }, origin) => {
      if (origin === 'local' && ws && ws.readyState === WebSocket.OPEN) {
        const changedClients = added.concat(updated).concat(removed);
        const encoder = encoding.createEncoder();
        encoding.writeVarUint(encoder, MESSAGE_AWARENESS);
        encoding.writeVarUint8Array(encoder, awarenessProtocol.encodeAwarenessUpdate(awareness, changedClients));
        ws.send(encoding.toUint8Array(encoder));
      }
    };
    awareness.on('update', awarenessUpdateHandler);
    
    // WebSocket connection with auto-reconnect
    const connect = () => {
      if (!isMounted) return;
      
      const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const host = window.location.host;
      const wsUrl = `${protocol}//${host}/api/notes/${note.id}/ws?token=${encodeURIComponent(token)}`;
      
      ws = new WebSocket(wsUrl);
      ws.binaryType = 'arraybuffer';
      wsRef.current = ws;
      
      ws.onopen = () => {
        if (!isMounted) return;
        setWsConnected(true);
        
        // Send Yjs sync step 1
        const encoder = encoding.createEncoder();
        encoding.writeVarUint(encoder, MESSAGE_SYNC);
        syncProtocol.writeSyncStep1(encoder, ydoc);
        ws.send(encoding.toUint8Array(encoder));
        
        // Setup local awareness user info if not in read-only viewer mode
        if (!isViewer) {
          awareness.setLocalStateField('user', {
            name: displayName,
            color: userColor.color,
            colorLight: userColor.light,
          });
        } else {
          awarenessProtocol.removeAwarenessStates(awareness, [ydoc.clientID], 'local');
        }
      };
      
      ws.onclose = () => {
        if (isMounted) {
          setWsConnected(false);
          reconnectTimer = setTimeout(() => {
            if (isMounted) connect();
          }, 2000);
        }
      };
      
      ws.onerror = (err) => console.warn('WebSocket error:', err);
      
      ws.onmessage = (event) => {
        if (!isMounted) return;
        
        if (typeof event.data === 'string') {
          // JSON text frame
          try {
            const msg = JSON.parse(event.data);
            if (msg.type === 'presence') {
              if (msg.users) setCollaborators(msg.users);
            } else if (msg.type === 'saved' || msg.type === 'save') {
              if (onRefreshNote && note?.id) {
                onRefreshNote(note.id);
              }
            }
          } catch (err) {
            console.error('Failed to parse text WS message:', err);
          }
        } else {
          // Binary frame -- Yjs sync protocol
          const data = new Uint8Array(event.data);
          const decoder = decoding.createDecoder(data);
          const messageType = decoding.readVarUint(decoder);
          
          if (messageType === MESSAGE_SYNC) {
            const encoder = encoding.createEncoder();
            encoding.writeVarUint(encoder, MESSAGE_SYNC);
            const syncMessageType = syncProtocol.readSyncMessage(decoder, encoder, ydoc, 'remote');
            if (encoding.length(encoder) > 1) {
              ws.send(encoding.toUint8Array(encoder));
            }
          } else if (messageType === MESSAGE_AWARENESS) {
            awarenessProtocol.applyAwarenessUpdate(awareness, decoding.readVarUint8Array(decoder), 'remote');
          }
        }
      };
    };
    
    connect();
    
    // Initialize content from REST API if we're the first client (no peers to sync from)
    initTimer = setTimeout(() => {
      if (ytext.length === 0 && note.content) {
        ydoc.transact(() => {
          ytext.insert(0, note.content);
        });
      }
      if (ytitle.length === 0 && note.title) {
        ydoc.transact(() => {
          ytitle.insert(0, note.title);
        });
      }
      // Set initial title for React
      setTitle(ytitle.toString());
    }, 500);
    
    return () => {
      isMounted = false;
      if (initTimer) clearTimeout(initTimer);
      if (reconnectTimer) clearTimeout(reconnectTimer);
      ytitle.unobserve(titleObserver);
      ydoc.off('update', updateHandler);
      awareness.off('update', awarenessUpdateHandler);
      if (ws && ws.readyState === WebSocket.OPEN) {
        try {
          awarenessProtocol.removeAwarenessStates(awareness, [ydoc.clientID], 'local');
        } catch (e) {
          console.warn('Error removing awareness state on cleanup:', e);
        }
        ws.close();
      } else if (ws) {
        ws.close();
      }
      if (editorViewRef.current) {
        editorViewRef.current.destroy();
        editorViewRef.current = null;
      }
      awareness.destroy();
      ydoc.destroy();
    };
  }, [note?.id, token]);

  // Sync awareness state dynamically when viewer status changes
  useEffect(() => {
    if (!awarenessRef.current || !ydocRef.current) return;
    if (isViewer) {
      try {
        awarenessProtocol.removeAwarenessStates(awarenessRef.current, [ydocRef.current.clientID], 'local');
      } catch (e) {
        console.warn('Error removing awareness on viewer demotion:', e);
      }
    } else {
      awarenessRef.current.setLocalStateField('user', {
        name: displayName,
        color: userColor.color,
        colorLight: userColor.light,
      });
    }
  }, [isViewer, displayName, userColor.color, userColor.light]);
  
  // CodeMirror setup effect
  useEffect(() => {
    if (!editorContainerRef.current || !ydocRef.current || !awarenessRef.current || !note) return;
    if (editorViewRef.current) {
      editorViewRef.current.destroy();
    }
    
    const ytext = ydocRef.current.getText('content');
    const awareness = awarenessRef.current;
    
    const editorTheme = EditorView.theme({
      '&': {
        backgroundColor: 'var(--bg-card)',
        color: 'var(--text-primary)',
        fontFamily: 'var(--font-mono)',
        fontSize: '14px',
        height: '100%',
      },
      '.cm-content': {
        caretColor: 'var(--accent)',
        padding: '12px 0',
        fontFamily: 'var(--font-mono)',
      },
      '.cm-cursor': { borderLeftColor: 'var(--accent)' },
      '.cm-selectionBackground': { backgroundColor: 'rgba(217, 119, 6, 0.15)' },
      '&.cm-focused .cm-selectionBackground': { backgroundColor: 'rgba(217, 119, 6, 0.25)' },
      '.cm-activeLine': { backgroundColor: 'rgba(0, 0, 0, 0.03)' },
      '.cm-gutters': {
        backgroundColor: 'var(--bg-sidebar)',
        color: 'var(--text-muted)',
        border: 'none',
        borderRight: '1px solid var(--border-color)',
      },
      '.cm-activeLineGutter': { backgroundColor: 'rgba(0, 0, 0, 0.05)' },
      // Yjs remote cursor styling
      '.cm-ySelectionInfo': {
        fontSize: '11px',
        fontFamily: 'var(--font-sans)',
        padding: '1px 4px',
        borderRadius: '3px',
        opacity: '0.9',
      },
      '.cm-scroller': { overflow: 'auto' },
    });
    
    const state = EditorState.create({
      doc: ytext.toString(),
      extensions: [
        basicSetup,
        markdown(),
        keymap.of(yUndoManagerKeymap),
        yCollab(ytext, awareness, { undoManager: new Y.UndoManager(ytext) }),
        editorTheme,
        EditorView.editable.of(!isViewer),
        EditorState.readOnly.of(isViewer),
      ],
    });
    
    const view = new EditorView({
      state,
      parent: editorContainerRef.current,
    });
    
    editorViewRef.current = view;
    
    return () => {
      view.destroy();
      editorViewRef.current = null;
    };
  }, [note?.id, ydocRef.current, isViewer]);
  
  const handleSave = async () => {
    if (!note || isViewer || !ytextRef.current || !ytitleRef.current) return;
    setIsSaving(true);
    setSaveError(false);
    try {
      const currentTitle = ytitleRef.current.toString();
      const currentContent = ytextRef.current.toString();
      await onSaveNote(note.id, currentTitle, currentContent);
      setIsSaving(false);
      
      if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
        wsRef.current.send(JSON.stringify({
          type: 'save',
          title: currentTitle,
          content: currentContent,
        }));
      }
    } catch {
      setIsSaving(false);
      setSaveError(true);
    }
  };
  
  const handleTitleChange = (e) => {
    if (isViewer || !ytitleRef.current || !ydocRef.current) return;
    const newTitle = e.target.value;
    ydocRef.current.transact(() => {
      ytitleRef.current.delete(0, ytitleRef.current.length);
      ytitleRef.current.insert(0, newTitle);
    });
  };
  
  // For preview tab, read content from ytext
  useEffect(() => {
    if (activeTab === 'preview' && ytextRef.current) {
      setPreviewContent(ytextRef.current.toString());
    }
  }, [activeTab, docVersion]);
  
  const handleShareSubmit = async (e) => {
    e.preventDefault();
    if (!note || !shareEmail || !onShareNote) return;

    setSharing(true);
    setShareStatus(null);
    try {
      await onShareNote(note.id, shareEmail, sharePermission);
      setShareStatus({ type: 'success', text: `Note successfully shared with ${shareEmail} (${sharePermission} access)` });
      setShareEmail('');
      await fetchShares();
    } catch (err) {
      setShareStatus({ type: 'error', text: err.message || 'Failed to share note' });
    } finally {
      setSharing(false);
    }
  };

  const handleUpdateUserPermission = async (targetEmail, newPermission) => {
    if (!note || !onShareNote) return;
    try {
      await onShareNote(note.id, targetEmail, newPermission);
      await fetchShares();
    } catch (err) {
      alert('Failed to update permission: ' + err.message);
    }
  };

  const handleRemoveShare = async (targetUserId) => {
    if (!note) return;
    try {
      const res = await fetch(`/api/notes/${note.id}/shares/${targetUserId}`, {
        method: 'DELETE',
        headers: { Authorization: `Bearer ${token}` },
      });
      if (!res.ok) throw new Error('Failed to remove share');
      await fetchShares();
    } catch (err) {
      alert('Failed to remove share: ' + err.message);
    }
  };

  const handleFileChange = async (e) => {
    const file = e.target.files[0];
    if (!file) return;

    setUploading(true);
    try {
      const res = await fetch(`/api/notes/${note.id}/attachments`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({ filename: file.name }),
      });

      if (!res.ok) throw new Error('Failed to get upload URL');
      const { attachment_id, key, url } = await res.json();

      const uploadRes = await fetch(url, {
        method: 'PUT',
        body: file,
      });

      if (!uploadRes.ok) throw new Error('Failed to upload file to storage');

      const completeRes = await fetch(`/api/notes/${note.id}/attachments/complete`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          'Authorization': `Bearer ${token}`,
        },
        body: JSON.stringify({
          attachment_id,
          key,
          filename: file.name,
          size: file.size,
        }),
      });

      if (!completeRes.ok) throw new Error('Failed to complete attachment');

      if (onRefreshNote) {
        await onRefreshNote(note.id);
      }
    } catch (err) {
      console.error('Upload failed:', err);
      alert('Upload failed: ' + err.message);
    } finally {
      setUploading(false);
      e.target.value = '';
    }
  };

  const handleDeleteAttachment = async (attachmentId) => {
    if (!note) return;
    setDeletingId(attachmentId);
    try {
      const res = await fetch(`/api/notes/${note.id}/attachments/${attachmentId}`, {
        method: 'DELETE',
        headers: {
          'Authorization': `Bearer ${token}`,
        },
      });

      if (!res.ok) {
        throw new Error('Failed to delete attachment');
      }

      if (onRefreshNote) {
        await onRefreshNote(note.id);
      }
    } catch (err) {
      console.error('Failed to delete attachment:', err);
      alert('Failed to delete attachment: ' + err.message);
    } finally {
      setDeletingId(null);
    }
  };

  const renderMarkdown = (text) => {
    if (!text) return '<p style="color: var(--text-muted)">Nothing to preview</p>';
    try {
      return marked.parse(text, { gfm: true, breaks: true });
    } catch (err) {
      console.error(err);
      return '<p style="color: var(--error)">Error rendering markdown</p>';
    }
  };

  if (!note) {
    return (
      <div className="placeholder-pane">
        <span className="placeholder-icon">📓</span>
        <h2>No Note Selected</h2>
        <p>Choose a note from the sidebar or create a new one to begin.</p>
      </div>
    );
  }

  const attachmentCount = note.attachments ? note.attachments.length : 0;

  return (
    <div className="editor-pane animate-fade-in" style={{ display: 'flex', flexDirection: 'column', height: '100%', padding: 0 }}>
      {/* PDF Export Result Modal */}
      {pdfExportResult && (
        <div className="modal-overlay">
          <div className="modal-card animate-fade-in" style={{ maxWidth: '480px', width: '90%' }}>
            <div className="modal-header">
              <h3>PDF Export Ready</h3>
              <button
                type="button"
                className="btn-icon"
                onClick={() => setPdfExportResult(null)}
              >
                ✕
              </button>
            </div>
            <div className="modal-body" style={{ textAlign: 'center', padding: '24px 16px' }}>
              <div style={{ fontSize: '44px', marginBottom: '12px' }}>📄</div>
              <h4 style={{ margin: '0 0 8px 0', fontSize: '16px', color: 'var(--text-primary)', fontWeight: 600 }}>
                {pdfExportResult.filename}
              </h4>
              <p style={{ margin: 0, fontSize: '14px', color: 'var(--text-secondary)', lineHeight: 1.5 }}>
                Your note has been exported to PDF and downloaded to your computer.
              </p>
            </div>
            <div className="modal-footer" style={{ justifyContent: 'space-between' }}>
              {pdfExportResult.pdf_base64 && (
                <button
                  type="button"
                  className="btn btn-secondary"
                  onClick={() => downloadPdfFile(pdfExportResult.pdf_base64, pdfExportResult.filename)}
                >
                  Download Again
                </button>
              )}
              <button
                type="button"
                className="btn btn-primary"
                onClick={() => setPdfExportResult(null)}
              >
                Done
              </button>
            </div>
          </div>
        </div>
      )}

      {/* Share Note Modal */}
      {showShareModal && (
        <div className="modal-overlay">
          <div className="modal-card animate-fade-in" style={{ maxWidth: '520px', width: '90%' }}>
            <div className="modal-header">
              <h3>Share Note</h3>
              <button
                type="button"
                className="btn-icon"
                onClick={() => setShowShareModal(false)}
              >
                ✕
              </button>
            </div>
            <form onSubmit={handleShareSubmit}>
              <div className="modal-body">
                <p style={{ fontSize: '14px', color: 'var(--text-secondary)', marginBottom: '12px' }}>
                  Share <strong>"{note.title}"</strong> with other users:
                </p>
                <div style={{ display: 'flex', gap: '8px', marginBottom: '8px' }}>
                  <input
                    type="email"
                    className="search-input"
                    placeholder="recipient@example.com"
                    value={shareEmail}
                    onChange={(e) => setShareEmail(e.target.value)}
                    style={{ flex: 1 }}
                    required
                  />
                  <select
                    className="search-input"
                    value={sharePermission}
                    onChange={(e) => setSharePermission(e.target.value)}
                    style={{ width: '110px', cursor: 'pointer' }}
                  >
                    <option value="editor">Editor</option>
                    <option value="viewer">Viewer</option>
                  </select>
                </div>

                {shareStatus && (
                  <div
                    style={{
                      fontSize: '13px',
                      color: shareStatus.type === 'success' ? 'var(--accent)' : 'var(--error)',
                      marginBottom: '12px',
                    }}
                  >
                    {shareStatus.text}
                  </div>
                )}

                {sharedUsers.length > 0 && (
                  <div style={{ marginTop: '16px', borderTop: '1px solid var(--border-color)', paddingTop: '12px' }}>
                    <h4 style={{ fontSize: '13px', color: 'var(--text-muted)', marginBottom: '8px', textTransform: 'uppercase', letterSpacing: '0.5px' }}>
                      People with access
                    </h4>
                    <div style={{ display: 'flex', flexDirection: 'column', gap: '8px', maxHeight: '160px', overflowY: 'auto' }}>
                      {sharedUsers.map((usr) => (
                        <div key={usr.user_id} style={{ display: 'flex', alignItems: 'center', justifyContent: 'space-between', padding: '6px 10px', background: 'var(--bg-sidebar)', borderRadius: '6px' }}>
                          <span style={{ fontSize: '13px', color: 'var(--text-primary)', textOverflow: 'ellipsis', overflow: 'hidden', whiteSpace: 'nowrap', maxWidth: '240px' }} title={usr.email}>
                            {usr.email}
                          </span>
                          <div style={{ display: 'flex', gap: '8px', alignItems: 'center' }}>
                            <select
                              value={usr.permission}
                              onChange={(e) => handleUpdateUserPermission(usr.email, e.target.value)}
                              style={{ fontSize: '12px', padding: '2px 6px', borderRadius: '4px', background: 'var(--bg-card)', color: 'var(--text-primary)', border: '1px solid var(--border-color)', cursor: 'pointer' }}
                            >
                              <option value="editor">Editor</option>
                              <option value="viewer">Viewer</option>
                            </select>
                            <button
                              type="button"
                              onClick={() => handleRemoveShare(usr.user_id)}
                              style={{ background: 'none', border: 'none', color: 'var(--error)', cursor: 'pointer', fontSize: '14px', padding: '2px' }}
                              title="Remove access"
                            >
                              ✕
                            </button>
                          </div>
                        </div>
                      ))}
                    </div>
                  </div>
                )}
              </div>
              <div className="modal-footer">
                <button
                  type="button"
                  className="btn btn-secondary"
                  onClick={() => setShowShareModal(false)}
                >
                  Close
                </button>
                <button
                  type="submit"
                  className="btn btn-primary"
                  disabled={sharing || !shareEmail}
                >
                  {sharing ? 'Sharing...' : 'Share Note'}
                </button>
              </div>
            </form>
          </div>
        </div>
      )}

      {/* Editor Header Bar */}
      <div className="editor-header" style={{ padding: '12px 24px' }}>
        <div className="editor-status" style={{ display: 'flex', gap: '12px', alignItems: 'center' }}>
          {isViewer ? (
            <span style={{ color: 'var(--text-muted)', fontWeight: 500 }}>👁️ Read-Only Mode</span>
          ) : saveStatus === 'Saving...' ? (
            <span style={{ color: 'var(--accent)' }}>⏳ Saving...</span>
          ) : saveStatus === 'Unsaved changes' ? (
            <span style={{ color: 'var(--error)' }}>● Unsaved changes</span>
          ) : saveStatus === 'Error saving' ? (
            <span style={{ color: 'var(--error)' }}>⚠️ Error saving</span>
          ) : (
            <span style={{ color: 'var(--text-muted)' }}>✓ Saved</span>
          )}

          {wsConnected && (
            <span style={{ fontSize: '12px', color: 'var(--accent)', background: 'rgba(217, 119, 6, 0.1)', padding: '2px 8px', borderRadius: '12px', fontWeight: 500 }}>
              ⚡ Live Sync
            </span>
          )}

          {collaborators.length > 0 && (
            <div style={{ display: 'flex', gap: '4px', alignItems: 'center' }}>
              {collaborators.map((c) => (
                <span key={c.user_id} title={`Collaborator: ${c.email}`} style={{ fontSize: '11px', background: 'var(--accent)', color: '#fff', padding: '2px 6px', borderRadius: '10px' }}>
                  👤 {c.email ? c.email.split('@')[0] : 'User'}
                </span>
              ))}
            </div>
          )}
        </div>
        
        <div style={{ display: 'flex', gap: '10px', alignItems: 'center' }}>
          {!isViewer && (
            <button
              type="button"
              className="btn btn-secondary"
              onClick={() => {
                setShareStatus(null);
                setShareEmail('');
                setShowShareModal(true);
              }}
            >
              🔗 Share Note
            </button>
          )}

          <button
            type="button"
            className="btn btn-secondary"
            onClick={handleExportPdf}
            disabled={exportingPdf}
            style={{ backgroundColor: 'var(--bg-sidebar)' }}
          >
            {exportingPdf ? '⏳ Exporting...' : '📄 Export PDF'}
          </button>

          <button
            type="button"
            className={`btn ${showAttachments ? 'btn-primary' : ''}`}
            style={{ backgroundColor: showAttachments ? 'var(--text-primary)' : 'var(--bg-sidebar)' }}
            onClick={() => setShowAttachments(!showAttachments)}
          >
            📎 Attachments {attachmentCount > 0 && `(${attachmentCount})`}
          </button>
          
          {!isViewer && (
            <button 
              type="button" 
              className="btn btn-primary"
              onClick={handleSave}
              disabled={saveStatus === 'All changes saved' || saveStatus === 'Saving...'}
            >
              Save Note
            </button>
          )}
        </div>
      </div>

      {/* Editor Workspace Split Layout */}
      <div className="editor-main-layout" style={{ display: 'flex', flex: 1, overflow: 'hidden' }}>
        {/* Left Side: Code/Markdown Editor */}
        <div style={{ flex: 1, display: 'flex', flexDirection: 'column', overflow: 'hidden', padding: '0 24px 24px 24px' }}>
          <div className="editor-tabs" style={{ marginTop: '12px' }}>
            <button 
              type="button"
              className={`editor-tab ${activeTab === 'edit' ? 'active' : ''}`}
              onClick={() => setActiveTab('edit')}
            >
              Edit Code/Markdown
            </button>
            <button 
              type="button"
              className={`editor-tab ${activeTab === 'preview' ? 'active' : ''}`}
              onClick={() => setActiveTab('preview')}
            >
              Visual Preview
            </button>
            <button 
              type="button"
              className={`editor-tab ${activeTab === 'comments' ? 'active' : ''}`}
              onClick={() => setActiveTab('comments')}
            >
              Comments
            </button>
          </div>

          <input
            type="text"
            className="editor-title-input"
            value={title}
            onChange={handleTitleChange}
            placeholder="Note Title"
            readOnly={isViewer}
          />

          <div className="editor-content-area" style={{ flex: 1, display: 'flex', flexDirection: 'column', overflowY: 'auto' }}>
            <div 
              ref={editorContainerRef} 
              style={{ flex: 1, display: activeTab === 'edit' ? 'flex' : 'none', flexDirection: 'column' }} 
            />
            {activeTab === 'preview' && (
              <div 
                className="preview-pane"
                dangerouslySetInnerHTML={{ __html: renderMarkdown(previewContent) }}
                style={{ flex: 1 }}
              />
            )}
            {activeTab === 'comments' && (
              <div className="comments-pane" style={{ flex: 1, padding: '16px', display: 'flex', flexDirection: 'column', gap: '16px' }}>
                {gqlData?.author && (
                  <div style={{ padding: '8px 12px', backgroundColor: 'var(--bg-sidebar)', borderRadius: '8px', border: '1px solid var(--border-color)', fontSize: '13px', color: 'var(--text-muted)' }}>
                    Note Author: <strong style={{ color: 'var(--text-main)' }}>{gqlData.author.email}</strong>
                  </div>
                )}

                <div style={{ flex: 1, overflowY: 'auto', display: 'flex', flexDirection: 'column', gap: '12px' }}>
                  {gqlLoading ? (
                    <div style={{ color: 'var(--text-muted)', fontSize: '13px' }}>Loading GraphQL comments...</div>
                  ) : gqlData?.comments && gqlData.comments.length > 0 ? (
                    gqlData.comments.map((comment) => (
                      <div key={comment.id} style={{ padding: '12px', borderRadius: '8px', backgroundColor: 'var(--bg-card)', border: '1px solid var(--border-color)' }}>
                        <div style={{ display: 'flex', justifyContent: 'space-between', marginBottom: '4px', fontSize: '12px', color: 'var(--text-muted)' }}>
                          <span><strong>{comment.author?.email || 'User'}</strong></span>
                          <span>{comment.createdAt ? new Date(comment.createdAt).toLocaleString() : ''}</span>
                        </div>
                        <div style={{ fontSize: '14px', color: 'var(--text-main)', whiteSpace: 'pre-wrap' }}>{comment.content}</div>
                      </div>
                    ))
                  ) : (
                    <div style={{ color: 'var(--text-muted)', fontSize: '13px', fontStyle: 'italic' }}>No comments yet on this note.</div>
                  )}
                </div>

                <form onSubmit={handleAddComment} style={{ display: 'flex', gap: '8px' }}>
                  <input
                    type="text"
                    value={newCommentText}
                    onChange={(e) => setNewCommentText(e.target.value)}
                    placeholder="Add a comment..."
                    style={{ flex: 1, padding: '8px 12px', borderRadius: '6px', border: '1px solid var(--border-color)', backgroundColor: 'var(--bg-card)', color: 'var(--text-main)' }}
                    disabled={submittingComment}
                  />
                  <button type="submit" className="btn btn-primary" disabled={submittingComment || !newCommentText.trim()}>
                    {submittingComment ? 'Posting...' : 'Post Comment'}
                  </button>
                </form>
              </div>
            )}
          </div>
        </div>

        {/* Right Side: Collapsible Attachments Sidebar */}
        {showAttachments && (
          <aside className="attachments-sidebar animate-fade-in">
            <div className="attachments-sidebar-header">
              <h3>Attachments ({attachmentCount})</h3>
              <button 
                type="button" 
                className="btn-icon" 
                onClick={() => setShowAttachments(false)}
                title="Close attachments panel"
              >
                ✕
              </button>
            </div>

            <div style={{ marginBottom: '16px' }}>
              <label className="btn" style={{ width: '100%', cursor: 'pointer', backgroundColor: 'var(--bg-sidebar)', borderColor: 'var(--border-color)', justifyContent: 'center' }}>
                {uploading ? 'Uploading...' : '+ Attach File'}
                <input
                  type="file"
                  onChange={handleFileChange}
                  disabled={uploading}
                  style={{ display: 'none' }}
                />
              </label>
            </div>

            {uploading && (
              <div style={{ padding: '8px 0', color: 'var(--accent)', fontSize: '12px', textAlign: 'center' }}>
                ⏳ Uploading to S3 storage...
              </div>
            )}

            <div className="attachments-sidebar-list">
              {note.attachments && note.attachments.length > 0 ? (
                note.attachments.map((att) => {
                  const isImage = att.filename.match(/\.(jpeg|jpg|gif|png|webp|svg)$/i);
                  const formattedSize = (att.size / 1024).toFixed(1) + ' KB';
                  const isDeleting = deletingId === att.id;

                  return (
                    <div key={att.id} className="attachment-sidebar-item">
                      <div className="attachment-sidebar-thumb">
                        {isImage ? (
                          <img src={att.url} alt={att.filename} />
                        ) : (
                          <span>📄</span>
                        )}
                      </div>

                      <div className="attachment-sidebar-details">
                        <span className="attachment-sidebar-name" title={att.filename}>
                          {att.filename}
                        </span>
                        <span className="attachment-sidebar-size">{formattedSize}</span>
                        <a 
                          href={att.url} 
                          target="_blank" 
                          rel="noreferrer" 
                          className="attachment-action-btn"
                          style={{ textDecoration: 'none', display: 'inline-block', marginTop: '2px' }}
                        >
                          Open File
                        </a>
                      </div>

                      <button
                        type="button"
                        className="attachment-delete-btn"
                        onClick={() => handleDeleteAttachment(att.id)}
                        disabled={isDeleting}
                        title="Delete attachment"
                      >
                        {isDeleting ? '...' : '🗑️'}
                      </button>
                    </div>
                  );
                })
              ) : (
                <div style={{ color: 'var(--text-muted)', fontSize: '13px', fontStyle: 'italic', textAlign: 'center', padding: '24px 0' }}>
                  No attachments yet. Click "+ Attach File" above.
                </div>
              )}
            </div>
          </aside>
        )}
      </div>
    </div>
  );
}
