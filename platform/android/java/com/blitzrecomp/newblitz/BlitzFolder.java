package com.blitzrecomp.newblitz;

import android.database.Cursor;
import android.database.MatrixCursor;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

/**
 * New Blitz's folder in the system's Files app (1.0.6): Add-Ons' Open Folder opens it there (BlitzActivity.openAppFolder), and
 * it's listed in the Files app's sidebar and in every file picker as "New Blitz". It's the app folder the game uses
 * (Android/data/<package>/files: the ROM, packs/, uniforms/, art/, paks/, exports/ ...), which file managers can't reach on
 * their own since Android 11. Reading, adding, renaming and deleting, all inside that folder; only the system's documents UI
 * can bind to it (MANAGE_DOCUMENTS in the manifest).
 */
public class BlitzFolder extends DocumentsProvider {
    static final String ROOT = "newblitz";
    static final String ROOT_DOC = "app";

    private static final String[] ROOT_COLUMNS = {Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_SUMMARY,
        Root.COLUMN_DOCUMENT_ID, Root.COLUMN_ICON, Root.COLUMN_AVAILABLE_BYTES};
    private static final String[] DOC_COLUMNS = {Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_LAST_MODIFIED, Document.COLUMN_FLAGS, Document.COLUMN_SIZE};

    private File base() {
        File app = getContext().getExternalFilesDir(null);
        if (app == null) app = new File(getContext().getFilesDir(), "app");  // as BlitzActivity.onCreate
        app.mkdirs();
        return app;
    }

    private String idOf(File f) throws FileNotFoundException {
        try {
            String b = base().getCanonicalPath(), p = f.getCanonicalPath();
            if (p.equals(b)) return ROOT_DOC;
            if (p.startsWith(b + "/")) return ROOT_DOC + p.substring(b.length());
        } catch (IOException e) {
            // below
        }
        throw new FileNotFoundException("outside New Blitz's folder: " + f);
    }

    /** The file a document id names, never outside the folder (no "..", no links out). */
    private File fileOf(String id) throws FileNotFoundException {
        if (id == null || !(id.equals(ROOT_DOC) || id.startsWith(ROOT_DOC + "/"))) throw new FileNotFoundException(id);
        File f = new File(base(), id.substring(ROOT_DOC.length()));
        idOf(f);  // throws when it resolves outside
        return f;
    }

    private static String mimeOf(File f) {
        if (f.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = f.getName();
        int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase(java.util.Locale.US));
            if (m != null) return m;
        }
        return "application/octet-stream";
    }

    private void row(MatrixCursor c, File f) throws FileNotFoundException {
        String id = idOf(f);
        int flags = 0;
        if (f.isDirectory()) flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
        else flags |= Document.FLAG_SUPPORTS_WRITE;
        if (!id.equals(ROOT_DOC)) flags |= Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        c.newRow()
            .add(Document.COLUMN_DOCUMENT_ID, id)
            .add(Document.COLUMN_DISPLAY_NAME, id.equals(ROOT_DOC) ? "New Blitz" : f.getName())
            .add(Document.COLUMN_MIME_TYPE, mimeOf(f))
            .add(Document.COLUMN_LAST_MODIFIED, f.lastModified())
            .add(Document.COLUMN_FLAGS, flags)
            .add(Document.COLUMN_SIZE, f.isDirectory() ? null : f.length());
    }

    @Override public boolean onCreate() { return true; }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        File b = base();
        // BRANDING: the Files app's name for the app folder
        c.newRow()
            .add(Root.COLUMN_ROOT_ID, ROOT)
            .add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_LOCAL_ONLY | Root.FLAG_SUPPORTS_IS_CHILD)
            .add(Root.COLUMN_TITLE, "New Blitz")
            .add(Root.COLUMN_SUMMARY, "ROM, add-ons and saves")
            .add(Root.COLUMN_DOCUMENT_ID, ROOT_DOC)
            .add(Root.COLUMN_ICON, getContext().getApplicationInfo().icon)
            .add(Root.COLUMN_AVAILABLE_BYTES, b.getFreeSpace());
        return c;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        row(c, fileOf(documentId));
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOC_COLUMNS);
        File[] files = fileOf(parentDocumentId).listFiles();
        if (files != null) for (File f : files) if (!f.getName().startsWith(".")) row(c, f);
        return c;
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal) throws FileNotFoundException {
        File f = fileOf(documentId);
        if (f.isDirectory()) throw new FileNotFoundException("a folder: " + documentId);
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.parseMode(mode));
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName) throws FileNotFoundException {
        File dir = fileOf(parentDocumentId);
        String name = displayName.replace('/', '_');
        if (name.equals(".") || name.equals("..") || name.isEmpty()) name = "file";
        File f = new File(dir, name);
        for (int n = 2; f.exists(); n++) f = new File(dir, name + " (" + n + ")");
        try {
            boolean ok = Document.MIME_TYPE_DIR.equals(mimeType) ? f.mkdir() : f.createNewFile();
            if (!ok) throw new FileNotFoundException("couldn't make " + f);
        } catch (IOException e) {
            throw new FileNotFoundException(e.toString());
        }
        return idOf(f);
    }

    private static void deleteTree(File f) {
        File[] kids = f.isDirectory() ? f.listFiles() : null;
        if (kids != null) for (File k : kids) deleteTree(k);
        f.delete();
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        if (ROOT_DOC.equals(documentId)) throw new FileNotFoundException("the folder itself stays");
        deleteTree(fileOf(documentId));
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        if (ROOT_DOC.equals(documentId)) throw new FileNotFoundException("the folder itself keeps its name");
        File f = fileOf(documentId);
        File to = new File(f.getParentFile(), displayName.replace('/', '_'));
        idOf(to);
        if (!f.renameTo(to)) throw new FileNotFoundException("couldn't rename " + f);
        return idOf(to);
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        return documentId != null && parentDocumentId != null && (documentId.startsWith(parentDocumentId + "/"));
    }

    @Override
    public String getDocumentType(String documentId) throws FileNotFoundException {
        return mimeOf(fileOf(documentId));
    }
}
